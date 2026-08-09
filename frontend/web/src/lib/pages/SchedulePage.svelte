<script lang="ts">
  import { streamProfileStore } from "$lib/stores/streamProfileStore.svelte";
  import { scheduleStore } from "$lib/stores/scheduleStore.svelte";
  import type { ScheduleState } from "$lib/api/bridge";
  import PageShell from "$lib/ui/PageShell.svelte";
  import Modal from "$lib/ui/Modal.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import Segmented, { type SegmentedOption } from "$lib/ui/Segmented.svelte";
  import HistoryList from "$lib/pages/history/HistoryList.svelte";
  import { pad2 } from "$lib/utils/format";

  // The calendar stays visible in both views; only the right-hand pane swaps.
  // Segmented is the same control GoLiveModal and OverlaysPage use, rather than a
  // fourth hand-rolled tab strip.
  const VIEW_OPTIONS: SegmentedOption[] = [
    { label: "Upcoming", value: "upcoming" },
    { label: "History", value: "history" },
  ];
  let view = $state<"upcoming" | "history">("upcoming");

  // Entries persist through schedule.* into the history database. What does NOT
  // exist yet is the runner: nothing arms an entry, counts down, or goes live at
  // its time, so `state` only ever moves when the app sweeps missed entries at
  // startup. A planned entry is a durable note, not yet an instruction.

  // The calendar's view of one entry. Kept flat and pre-split into date/time
  // strings because every derivation below groups by day, and re-deriving the
  // local date from an epoch inside each of them is where off-by-one-day bugs
  // come from.
  interface SchedEntry {
    id: string;
    date: string; // "YYYY-MM-DD", local
    time: string; // "19:00", local
    title: string;
    dur: string; // "4h" / "90m"
    tags: string[]; // destination labels
    state: ScheduleState;
    editable: boolean;
  }

  const MONTH_NAMES = [
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
  ];
  const MONTH_ABBR = [
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
  ];
  const WEEKDAYS = ["MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"];

  const iso = (y: number, m: number, d: number): string => `${y}-${pad2(m + 1)}-${pad2(d)}`;

  // Real "now" -- the Svelte app may use Date freely (the mock avoided it only for
  // its static sandbox). Captured once at mount; good enough for a planning view.
  const now = new Date();
  const todayStr = iso(now.getFullYear(), now.getMonth(), now.getDate());

  // Currently viewed month (defaults to the real current month; ◀/▶ navigate).
  let viewYear = $state(now.getFullYear());
  let viewMonth = $state(now.getMonth()); // 0-based

  // Duration is stored as minutes and shown the way a streamer says it. Round
  // hours read as hours; anything else keeps its minutes rather than being
  // rounded into a lie about a 90-minute slot.
  function formatDuration(min: number): string {
    if (min <= 0) {
      return "—";
    }
    if (min % 60 === 0) {
      return `${min / 60}h`;
    }
    return min < 60 ? `${min}m` : `${Math.floor(min / 60)}h ${min % 60}m`;
  }

  // Accepts "4h", "90m", "1h30", "2" (bare number = hours, since that is what
  // people mean when they type it into a stream planner).
  function parseDuration(text: string): number {
    const s = text.trim().toLowerCase();
    if (s === "") {
      return 60;
    }
    const h = /(\d+(?:\.\d+)?)\s*h/.exec(s);
    const m = /(\d+)\s*m/.exec(s);
    if (h || m) {
      return Math.max(1, Math.round((h ? parseFloat(h[1]) * 60 : 0) + (m ? parseInt(m[1], 10) : 0)));
    }
    const bare = parseFloat(s);
    return Number.isFinite(bare) ? Math.max(1, Math.round(bare * 60)) : 60;
  }

  // Local, not UTC: a calendar shows the user's own days. Deriving these from the
  // epoch once here keeps every grouping below on the same footing.
  function localDate(ms: number): string {
    const d = new Date(ms);
    return iso(d.getFullYear(), d.getMonth(), d.getDate());
  }
  function localTime(ms: number): string {
    const d = new Date(ms);
    return `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
  }
  function epochFrom(date: string, time: string): number {
    const [y, m, d] = date.split("-").map(Number);
    const [hh, mm] = time.split(":").map(Number);
    return new Date(y, m - 1, d, hh || 0, mm || 0, 0, 0).getTime();
  }

  scheduleStore.start();

  // Only planned and armed entries can still be edited; the bridge refuses the
  // rest, so the calendar must not offer it either.
  const EDITABLE_STATES: ScheduleState[] = ["planned", "armed"];

  let schedule = $derived.by<SchedEntry[]>(() =>
    scheduleStore.entries.map((e) => ({
      id: e.id,
      date: localDate(e.startsAt),
      time: localTime(e.startsAt),
      title: e.title || "Untitled Stream",
      dur: formatDuration(e.durationMin),
      tags: e.destinations.map((d) => profileLabel(d.profileId)),
      state: e.state,
      editable: EDITABLE_STATES.includes(e.state),
    })),
  );

  // --- real stream-profile list for the modal chips -----------------------------
  // Sourced from the shared store (one source of truth); read-only here. Canvases
  // are deliberately not selected on an entry: a destination is a stream profile,
  // and which canvases go live for it is already decided by that profile's output
  // bindings. Asking twice would let the two answers disagree.
  streamProfileStore.start();
  let profiles = $derived(streamProfileStore.profiles);

  // Destinations are chosen by profile id and shown by label, so two profiles
  // sharing a label cannot collapse into one selection. No fallback list: with no
  // profiles connected the modal shows an empty state, because offering
  // destinations that do not exist is how you schedule a broadcast to nowhere.
  let destOptions = $derived(profiles.map((p) => ({ id: p.uuid, label: p.label })));

  function profileLabel(id: string): string {
    // Falls back to the stored id so a destination whose profile was deleted
    // stays visible on past entries instead of silently vanishing from them.
    return profiles.find((p) => p.uuid === id)?.label ?? id;
  }

  // --- calendar derivation ------------------------------------------------------
  const calLabel = $derived(`${MONTH_NAMES[viewMonth]} ${viewYear}`);

  interface CalCell {
    key: string;
    day: string;
    date: string | null;
    inMonth: boolean;
    isToday: boolean;
    events: SchedEntry[];
  }

  let calCells = $derived.by<CalCell[]>(() => {
    const startDow = (new Date(viewYear, viewMonth, 1).getDay() + 6) % 7; // Mon-first
    const dim = new Date(viewYear, viewMonth + 1, 0).getDate();
    const byDay = new Map<number, SchedEntry[]>();
    for (const e of schedule) {
      const [ey, em, ed] = e.date.split("-").map(Number);
      if (ey === viewYear && em - 1 === viewMonth) {
        const arr = byDay.get(ed) ?? [];
        arr.push(e);
        byDay.set(ed, arr);
      }
    }
    const totalCells = Math.ceil((startDow + dim) / 7) * 7;
    const cells: CalCell[] = [];
    for (let i = 0; i < totalCells; i++) {
      const dayNum = i - startDow + 1;
      const inMonth = dayNum >= 1 && dayNum <= dim;
      const date = inMonth ? iso(viewYear, viewMonth, dayNum) : null;
      cells.push({
        key: `c${i}`,
        day: inMonth ? String(dayNum) : "",
        date,
        inMonth,
        isToday: date === todayStr,
        events: inMonth ? (byDay.get(dayNum) ?? []) : [],
      });
    }
    return cells;
  });

  // Count of streams planned in the viewed month (header sub).
  let monthCount = $derived(
    schedule.filter((e) => {
      const [ey, em] = e.date.split("-").map(Number);
      return ey === viewYear && em - 1 === viewMonth;
    }).length,
  );

  // Upcoming = entries today-or-later, soonest first.
  interface UpcomingItem extends SchedEntry {
    mon: string;
    dayNum: string;
  }
  let upcoming = $derived.by<UpcomingItem[]>(() =>
    [...schedule]
      .filter((e) => e.date >= todayStr)
      .sort((a, b) => (a.date < b.date ? -1 : a.date > b.date ? 1 : a.time < b.time ? -1 : 1))
      .map((e) => {
        const [, em, ed] = e.date.split("-").map(Number);
        return { ...e, mon: MONTH_ABBR[em - 1], dayNum: String(ed) };
      }),
  );

  function prevMonth(): void {
    if (viewMonth === 0) {
      viewMonth = 11;
      viewYear -= 1;
    } else {
      viewMonth -= 1;
    }
  }
  function nextMonth(): void {
    if (viewMonth === 11) {
      viewMonth = 0;
      viewYear += 1;
    } else {
      viewMonth += 1;
    }
  }

  // --- modal state --------------------------------------------------------------
  let modalOpen = $state(false);
  let mTitle = $state("Untitled Stream");
  let mDate = $state(todayStr);
  let mTime = $state("19:00");
  let mDur = $state("4h");
  let mNotes = $state("");
  // Profile ids, not labels: two profiles may share a label, and a selection keyed
  // by label would silently merge them.
  let mDests = $state<Set<string>>(new Set());
  // Empty when creating; set when editing an existing entry.
  let mEditingId = $state<string | null>(null);
  let mSaving = $state(false);
  let mError = $state<string | null>(null);

  function openModal(date: string): void {
    mEditingId = null;
    mTitle = "Untitled Stream";
    mDate = date;
    mTime = "19:00";
    mDur = "4h";
    mNotes = "";
    mError = null;
    // Default-select any primary destination, else the first.
    const primary = profiles.filter((p) => p.isPrimary).map((p) => p.uuid);
    mDests = new Set(primary.length > 0 ? primary : destOptions.slice(0, 1).map((d) => d.id));
    modalOpen = true;
  }

  function openEntry(entry: SchedEntry): void {
    if (!entry.editable) {
      return;
    }
    const stored = scheduleStore.entries.find((e) => e.id === entry.id);
    if (!stored) {
      return;
    }
    mEditingId = stored.id;
    mTitle = stored.title;
    mDate = localDate(stored.startsAt);
    mTime = localTime(stored.startsAt);
    mDur = formatDuration(stored.durationMin);
    mNotes = "";
    mError = null;
    mDests = new Set(stored.destinations.map((d) => d.profileId));
    modalOpen = true;
  }
  function closeModal(): void {
    modalOpen = false;
  }
  function toggle(set: Set<string>, label: string): Set<string> {
    const next = new Set(set);
    if (next.has(label)) {
      next.delete(label);
    } else {
      next.add(label);
    }
    return next;
  }

  // Writes through the bridge. The modal stays open on failure with the reason on
  // it: closing would discard what the user typed and leave them guessing why the
  // entry never appeared.
  async function scheduleStream(): Promise<void> {
    if (mSaving) {
      return;
    }
    mSaving = true;
    mError = null;
    const input = {
      startsAt: epochFrom(mDate, mTime),
      title: mTitle.trim() || "Untitled Stream",
      durationMin: parseDuration(mDur),
      announce: false,
      autoStart: false,
      destinations: [...mDests].map((profileId) => ({
        profileId,
        title: mTitle.trim() || "Untitled Stream",
        category: "",
        tags: [] as string[],
      })),
    };
    try {
      if (mEditingId) {
        await scheduleStore.update(mEditingId, input);
      } else {
        await scheduleStore.create(input);
      }
      await scheduleStore.refresh();
      // Jump the calendar to the entry's month so it is visible.
      const [ny, nm] = mDate.split("-").map(Number);
      if (!Number.isNaN(ny) && !Number.isNaN(nm)) {
        viewYear = ny;
        viewMonth = nm - 1;
      }
      modalOpen = false;
    } catch (e) {
      mError = (e as Error).message;
    } finally {
      mSaving = false;
    }
  }

  async function deleteEntry(): Promise<void> {
    if (!mEditingId || mSaving) {
      return;
    }
    mSaving = true;
    mError = null;
    try {
      await scheduleStore.remove(mEditingId);
      await scheduleStore.refresh();
      modalOpen = false;
    } catch (e) {
      mError = (e as Error).message;
    } finally {
      mSaving = false;
    }
  }
</script>

<PageShell title="Schedule" sub="Upcoming stream planning">
  {#snippet actions()}
    {#if scheduleStore.error}
      <span class="shell-note" title={scheduleStore.error}>scheduling unavailable</span>
    {/if}
    <div class="month-nav">
      <button class="nav-btn" title="Previous month" aria-label="Previous month" onclick={prevMonth}>
        <Icon name="caret-left" size={14} />
      </button>
      <span class="cal-label">{calLabel} · {monthCount} planned</span>
      <button class="nav-btn" title="Next month" aria-label="Next month" onclick={nextMonth}>
        <Icon name="caret-right" size={14} />
      </button>
    </div>
    <button class="new-btn" onclick={() => openModal(todayStr)}><Icon name="plus" size={13} /> New Stream</button>
  {/snippet}

  <div class="body">
    <div class="cal-wrap">
      <div class="weekrow">
        {#each WEEKDAYS as w (w)}
          <div class="weekday">{w}</div>
        {/each}
      </div>
      <div class="grid">
        {#each calCells as c (c.key)}
          <button
            type="button"
            class="cell"
            class:out={!c.inMonth}
            class:today={c.isToday}
            disabled={!c.inMonth}
            onclick={() => c.date && openModal(c.date)}
          >
            <div class="cell-head">
              <span class="cell-num">{c.day}</span>
            </div>
            {#each c.events as ev (ev.id)}
              <div class="chip-ev">
                <div class="chip-time">{ev.time}</div>
                <div class="chip-title">{ev.title}</div>
              </div>
            {/each}
          </button>
        {/each}
      </div>
    </div>

    <aside class="side">
      <div class="side-head">
        <Segmented
          options={VIEW_OPTIONS}
          value={view}
          onChange={(v) => (view = v as "upcoming" | "history")}
        />
      </div>
      <div class="side-list">
        {#if view === "history"}
          <HistoryList />
        {:else if upcoming.length === 0}
          <EmptyState
            compact
            title="No upcoming streams"
            sub={'Click a day or "+ New Stream".'}
          >
            {#snippet icon()}
              <Icon name="plus" size={22} />
            {/snippet}
          </EmptyState>
        {:else}
          {#each upcoming as u (u.id)}
            <button
              class="up-row"
              onclick={() => {
                const [uy, um] = u.date.split("-").map(Number);
                viewYear = uy;
                viewMonth = um - 1;
                openEntry(u);
              }}
            >
              <div class="up-date">
                <div class="up-mon">{u.mon}</div>
                <div class="up-day">{u.dayNum}</div>
              </div>
              <div class="up-body">
                <div class="up-title">{u.title}</div>
                <div class="up-meta">
                  {u.time} · {u.dur}{#if u.state !== "planned"} · {u.state}{/if}
                </div>
                {#if u.tags.length > 0}
                  <div class="up-tags">
                    {#each u.tags as t (t)}
                      <span class="up-tag">{t}</span>
                    {/each}
                  </div>
                {/if}
              </div>
            </button>
          {/each}
        {/if}
      </div>
    </aside>
  </div>
</PageShell>

{#if modalOpen}
  <Modal title="Schedule a Stream" onClose={closeModal} width={520}>
    <div class="sched-form">
      <div class="field">
        <div class="f-label">TITLE</div>
        <input class="f-input" bind:value={mTitle} spellcheck="false" />
      </div>
      <div class="field-row">
        <div class="field flex1">
          <div class="f-label">DATE</div>
          <input class="f-input" type="date" bind:value={mDate} />
        </div>
        <div class="field f-time">
          <div class="f-label">TIME</div>
          <input class="f-input" type="time" bind:value={mTime} />
        </div>
        <div class="field f-dur">
          <div class="f-label">DURATION</div>
          <input class="f-input" bind:value={mDur} spellcheck="false" />
        </div>
      </div>
      <div class="field">
        <div class="f-label">DESTINATIONS</div>
        {#if destOptions.length === 0}
          <p class="modal-note">
            No stream destinations are set up yet. Add one on the Destinations page and it
            will appear here.
          </p>
        {:else}
          <div class="chips">
            {#each destOptions as d (d.id)}
              <button
                type="button"
                class="chip"
                class:on={mDests.has(d.id)}
                onclick={() => (mDests = toggle(mDests, d.id))}
              >{d.label}</button>
            {/each}
          </div>
        {/if}
      </div>
      <div class="field">
        <div class="f-label">NOTES</div>
        <textarea class="f-input f-area" rows="3" bind:value={mNotes}
          placeholder="Go-live checklist, talking points, sponsor reads…"></textarea>
      </div>
      <p class="modal-note">
        Saved to your stream history. Braidcast will not go live on its own yet — the
        countdown and auto-start are still to come.
      </p>
      {#if mError}
        <p class="modal-error">{mError}</p>
      {/if}
    </div>

    {#snippet footer()}
      {#if mEditingId}
        <button class="ghost danger" onclick={deleteEntry} disabled={mSaving}>Delete</button>
      {/if}
      <button class="ghost" onclick={closeModal} disabled={mSaving}>Cancel</button>
      <button class="accent" onclick={scheduleStream} disabled={mSaving}>
        {mSaving ? "Saving…" : mEditingId ? "Save Changes" : "Schedule Stream"}
      </button>
    {/snippet}
  </Modal>
{/if}

<style>
  /* ---- header ---- */
  .month-nav {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .nav-btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 26px;
    height: 24px;
    padding: 0;
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
  }
  .nav-btn:hover {
    color: var(--color-accent);
    border-color: var(--color-accent);
  }
  .cal-label {
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-dim);
    min-width: 150px;
    text-align: center;
  }
  .shell-note {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: var(--color-accent);
    border: var(--border-weight) solid color-mix(in srgb, var(--color-accent) 50%, transparent);
    padding: 3px 7px;
    cursor: help;
  }
  .new-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    height: auto;
    padding: 8px 16px;
    font-size: 12px;
    font-weight: 600;
    border: 0;
    background: var(--color-accent);
    color: var(--color-accent-ink);
    font-family: var(--font-ui);
  }
  .new-btn:hover {
    border: 0;
    background: color-mix(in srgb, var(--color-accent) 88%, var(--color-text));
  }

  /* ---- body ---- */
  .body {
    flex: 1;
    min-height: 0;
    display: flex;
  }
  .cal-wrap {
    flex: 1;
    min-width: 0;
    min-height: 0;
    display: flex;
    flex-direction: column;
    overflow: auto;
    padding: 0;
  }
  .weekrow {
    flex: 0 0 auto;
    display: grid;
    grid-template-columns: repeat(7, 1fr);
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .weekday {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.1em;
    color: var(--color-dim);
    padding: 9px 12px;
    text-align: left;
  }
  .grid {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-columns: repeat(7, 1fr);
    grid-auto-rows: minmax(96px, 1fr);
    gap: var(--border-weight);
    background: var(--color-border);
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .cell {
    width: 100%;
    height: 100%;
    min-height: 0;
    background: var(--color-surface);
    padding: 5px 6px 7px;
    overflow: hidden;
    cursor: pointer;
    border: 0;
    text-align: left;
    display: flex;
    flex-direction: column;
    align-items: stretch;
    gap: 3px;
    font: inherit;
    color: inherit;
    transition: background 0.12s ease;
  }
  .cell:hover:not(.out) {
    background: color-mix(in srgb, var(--color-text) 4%, var(--color-surface));
  }
  .cell.out {
    background: var(--color-base);
    cursor: default;
  }
  .cell.today {
    background: color-mix(in srgb, var(--color-accent) 7%, var(--color-surface));
  }
  .cell:focus-visible {
    outline: 1px solid var(--color-accent);
    outline-offset: -2px;
  }
  .cell-head {
    display: flex;
    justify-content: flex-start;
    align-items: center;
  }
  .cell-num {
    font-family: var(--font-mono);
    font-size: 12px;
    color: var(--color-dim);
  }
  .cell.today .cell-num {
    font-weight: 600;
    color: var(--color-accent-ink);
    background: var(--color-accent);
    width: 22px;
    height: 22px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
  }
  .chip-ev {
    flex: 0 0 auto;
    width: 100%;
    display: flex;
    align-items: baseline;
    gap: 6px;
    padding: 3px 6px;
    background: color-mix(in srgb, var(--color-accent) 16%, transparent);
    border-left: 2px solid var(--color-accent);
    overflow: hidden;
  }
  .chip-time {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--color-accent);
  }
  .chip-title {
    font-size: 11px;
    color: var(--color-text);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  /* ---- upcoming sidebar ---- */
  .side {
    flex: 0 0 300px;
    border-left: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .side-head {
    flex: 0 0 auto;
    padding: 14px 16px;
    border-bottom: var(--border-weight) solid var(--color-border);
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.12em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .side-list {
    overflow: auto;
    flex: 1;
    min-height: 0;
  }
  .up-row {
    width: 100%;
    height: auto;
    display: flex;
    gap: 12px;
    padding: 14px 16px;
    border: 0;
    border-bottom: var(--border-weight) solid var(--color-border-2);
    background: transparent;
    text-align: left;
  }
  .up-row:hover {
    background: color-mix(in srgb, var(--color-text) 4%, transparent);
  }
  .up-date {
    flex: 0 0 auto;
    text-align: center;
    width: 40px;
  }
  .up-mon {
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--color-muted);
    letter-spacing: 0.06em;
  }
  .up-day {
    font-size: 16px;
    font-weight: 600;
    line-height: 1.1;
    color: var(--color-text);
  }
  .up-body {
    min-width: 0;
    flex: 1;
  }
  .up-title {
    font-size: 12px;
    font-weight: 500;
    margin-bottom: 3px;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .up-meta {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-dim);
  }
  .up-tags {
    margin-top: 6px;
    display: flex;
    flex-wrap: wrap;
    gap: 4px;
  }
  .up-tag {
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.06em;
    padding: 2px 6px;
    color: var(--color-dim);
    border: var(--border-weight) solid var(--color-border);
  }

  /* ---- modal ---- */
  .sched-form {
    display: flex;
    flex-direction: column;
    gap: 16px;
  }
  .field-row {
    display: flex;
    gap: 12px;
  }
  .flex1 {
    flex: 1;
  }
  .f-time {
    flex: 0 0 120px;
  }
  .f-dur {
    flex: 0 0 100px;
  }
  .f-label {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    color: var(--color-muted);
    margin-bottom: 6px;
  }
  .f-input {
    width: 100%;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 13px;
    padding: 9px 11px;
    outline: none;
  }
  .f-input:focus {
    border-color: var(--color-accent);
  }
  .f-area {
    resize: none;
  }
  .chips {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
  }
  .chip {
    height: auto;
    font-size: 11px;
    padding: 6px 12px;
    color: var(--color-dim);
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    font-family: var(--font-ui);
  }
  .chip:hover {
    border-color: var(--color-accent);
  }
  .chip.on {
    color: var(--color-accent-ink);
    background: var(--color-accent);
    border-color: var(--color-accent);
  }
  .modal-note {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 10px;
    line-height: 1.6;
    color: var(--color-muted);
  }

  .modal-error {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 10px;
    line-height: 1.6;
    color: var(--color-error);
  }

  .ghost.danger {
    color: var(--color-error);
  }
</style>
