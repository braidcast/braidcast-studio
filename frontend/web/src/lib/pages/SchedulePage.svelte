<script lang="ts">
  import { obs, type CanvasInfo, type StreamProfileInfo } from "../bridge";

  // SHELL ONLY (redesign Decision A): this page is a UI preview of the planned
  // scheduling feature. There is NO backend -- nothing is persisted (no
  // schedule.json, no bridge writes) and nothing auto-goes-live at the scheduled
  // time. The `schedule` array below lives purely in component memory: entries
  // added via the modal show on the calendar/Upcoming list but are lost on reload.
  // The calendar/Upcoming/modal layout mirrors the design mock; real canvas and
  // stream-profile lists populate the modal chips (read-only) so they reflect the
  // user's actual setup, falling back to placeholder labels when none exist.

  interface SchedEntry {
    id: number;
    date: string; // "YYYY-MM-DD"
    time: string; // "19:00"
    title: string;
    dur: string; // free text, e.g. "4h"
    tags: string[]; // destination labels
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

  const pad = (n: number): string => String(n).padStart(2, "0");
  const iso = (y: number, m: number, d: number): string => `${y}-${pad(m + 1)}-${pad(d)}`;

  // Real "now" -- the Svelte app may use Date freely (the mock avoided it only for
  // its static sandbox). Captured once at mount; good enough for a planning view.
  const now = new Date();
  const todayStr = iso(now.getFullYear(), now.getMonth(), now.getDate());

  // Currently viewed month (defaults to the real current month; ◀/▶ navigate).
  let viewYear = $state(now.getFullYear());
  let viewMonth = $state(now.getMonth()); // 0-based

  // Seed a couple of example entries in the current month so the calendar isn't
  // empty on first paint. In-memory only -- replaced/extended via the modal.
  function seedExamples(): SchedEntry[] {
    const y = now.getFullYear();
    const m = now.getMonth();
    const dim = new Date(y, m + 1, 0).getDate();
    const d = now.getDate();
    const clamp = (n: number): number => Math.min(Math.max(n, 1), dim);
    return [
      { id: 1, date: iso(y, m, clamp(d + 2)), time: "19:00", title: "Friday Night Ranked", dur: "4h", tags: ["Twitch", "YouTube"] },
      { id: 2, date: iso(y, m, clamp(d + 9)), time: "20:00", title: "Patch Notes Breakdown", dur: "2h", tags: ["YouTube"] },
      { id: 3, date: iso(y, m, clamp(d + 16)), time: "12:00", title: "Sunday Co-op", dur: "3h", tags: ["Twitch"] },
    ];
  }

  let schedule = $state<SchedEntry[]>(seedExamples());
  let nextId = $state(4);

  // --- real canvas + stream-profile lists for the modal chips (read-only) -------
  let canvases = $state<CanvasInfo[]>([]);
  let profiles = $state<StreamProfileInfo[]>([]);

  $effect(() => {
    obs.call("canvas.list").then((l) => (canvases = l)).catch(() => {});
    obs.call("streamProfile.list").then((l) => (profiles = l)).catch(() => {});
    const offCanvas = obs.on("canvas.changed", () => {
      obs.call("canvas.list").then((l) => (canvases = l)).catch(() => {});
    });
    const offProfiles = obs.on("streamProfile.changed", () => {
      obs.call("streamProfile.list").then((l) => (profiles = l)).catch(() => {});
    });
    return () => {
      offCanvas();
      offProfiles();
    };
  });

  // Chip option lists: real data when present, else placeholder labels from the
  // mock so the modal is never empty.
  const FALLBACK_CANVASES = ["Landscape", "Vertical"];
  const FALLBACK_DESTS = ["Twitch", "YouTube", "Kick", "TikTok", "Instagram"];

  let canvasOptions = $derived<string[]>(
    canvases.length > 0 ? canvases.map((c) => c.name) : FALLBACK_CANVASES,
  );
  let destOptions = $derived<string[]>(
    profiles.length > 0 ? profiles.map((p) => p.label) : FALLBACK_DESTS,
  );

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
  let mCanvases = $state<Set<string>>(new Set());
  let mDests = $state<Set<string>>(new Set());

  function openModal(date: string): void {
    mTitle = "Untitled Stream";
    mDate = date;
    mTime = "19:00";
    mDur = "4h";
    mNotes = "";
    // Default-select the first canvas + any primary destination (or the first).
    mCanvases = new Set(canvasOptions.slice(0, 1));
    if (profiles.length > 0) {
      const primary = profiles.filter((p) => p.isPrimary).map((p) => p.label);
      mDests = new Set(primary.length > 0 ? primary : destOptions.slice(0, 1));
    } else {
      mDests = new Set(destOptions.slice(0, 1));
    }
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

  // Push the drafted entry into the in-memory schedule (no persistence). The new
  // entry immediately shows on the calendar/Upcoming, then is lost on reload.
  function scheduleStream(): void {
    schedule = [
      ...schedule,
      {
        id: nextId,
        date: mDate,
        time: mTime,
        title: mTitle.trim() || "Untitled Stream",
        dur: mDur.trim() || "—",
        tags: [...mDests],
      },
    ];
    nextId += 1;
    // Jump the calendar to the new entry's month so it's visible.
    const [ny, nm] = mDate.split("-").map(Number);
    if (!Number.isNaN(ny) && !Number.isNaN(nm)) {
      viewYear = ny;
      viewMonth = nm - 1;
    }
    modalOpen = false;
  }

  function onKeydown(e: KeyboardEvent): void {
    if (modalOpen && e.key === "Escape") {
      closeModal();
    }
  }
</script>

<svelte:window onkeydown={onKeydown} />

<div class="page">
  <header class="head">
    <div class="head-left">
      <span class="title">Schedule</span>
      <div class="nav">
        <button class="nav-btn" title="Previous month" aria-label="Previous month" onclick={prevMonth}>◀</button>
        <span class="sub">{calLabel} · {monthCount} planned</span>
        <button class="nav-btn" title="Next month" aria-label="Next month" onclick={nextMonth}>▶</button>
      </div>
      <span class="shell-note" title="UI preview: scheduled streams are not saved to disk and do not auto-go-live yet.">
        preview · not saved
      </span>
    </div>
    <button class="new-btn" onclick={() => openModal(todayStr)}>+ New Stream</button>
  </header>

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
      <div class="side-head"><span>Upcoming</span></div>
      <div class="side-list">
        {#if upcoming.length === 0}
          <div class="side-empty">No upcoming streams. Click a day or "+ New Stream".</div>
        {:else}
          {#each upcoming as u (u.id)}
            <button
              class="up-row"
              onclick={() => {
                const [uy, um] = u.date.split("-").map(Number);
                viewYear = uy;
                viewMonth = um - 1;
                openModal(u.date);
              }}
            >
              <div class="up-date">
                <div class="up-mon">{u.mon}</div>
                <div class="up-day">{u.dayNum}</div>
              </div>
              <div class="up-body">
                <div class="up-title">{u.title}</div>
                <div class="up-meta">{u.time} · {u.dur}</div>
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
</div>

{#if modalOpen}
  <div
    class="backdrop"
    role="presentation"
    onclick={(e) => {
      if (e.target === e.currentTarget) {
        closeModal();
      }
    }}
  >
    <div class="modal" role="dialog" aria-modal="true" aria-label="Schedule a Stream">
      <div class="modal-head">
        <span class="modal-title">Schedule a Stream</span>
        <button class="modal-x" aria-label="Close" onclick={closeModal}>✕</button>
      </div>
      <div class="modal-body">
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
          <div class="f-label">CANVASES</div>
          <div class="chips">
            {#each canvasOptions as name (name)}
              <button
                type="button"
                class="chip"
                class:on={mCanvases.has(name)}
                onclick={() => (mCanvases = toggle(mCanvases, name))}
              >{name}</button>
            {/each}
          </div>
        </div>
        <div class="field">
          <div class="f-label">DESTINATIONS</div>
          <div class="chips">
            {#each destOptions as name (name)}
              <button
                type="button"
                class="chip"
                class:on={mDests.has(name)}
                onclick={() => (mDests = toggle(mDests, name))}
              >{name}</button>
            {/each}
          </div>
        </div>
        <div class="field">
          <div class="f-label">NOTES</div>
          <textarea class="f-input f-area" rows="3" bind:value={mNotes}
            placeholder="Go-live checklist, talking points, sponsor reads…"></textarea>
        </div>
        <p class="modal-note">
          Preview only — this schedule is kept in memory for layout review. It is not
          saved and will not start the stream automatically.
        </p>
      </div>
      <div class="modal-foot">
        <button class="btn" onclick={closeModal}>Cancel</button>
        <button class="btn primary" onclick={scheduleStream}>Schedule Stream</button>
      </div>
    </div>
  </div>
{/if}

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    background: var(--color-base);
    color: var(--color-text);
  }

  /* ---- header ---- */
  .head {
    flex: 0 0 auto;
    height: 58px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 24px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .head-left {
    display: flex;
    align-items: center;
    gap: 16px;
    min-width: 0;
  }
  .title {
    font-family: var(--font-ui);
    font-size: 16px;
    font-weight: 600;
    letter-spacing: -0.01em;
  }
  .nav {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .nav-btn {
    height: auto;
    padding: 2px 6px;
    font-size: 10px;
    line-height: 1;
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
  }
  .nav-btn:hover {
    color: var(--color-accent);
    border-color: var(--color-accent);
  }
  .sub {
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
    height: auto;
    padding: 9px 18px;
    font-size: 12px;
    font-weight: 600;
    border: 0;
    background: var(--color-accent);
    color: var(--color-accent-ink);
    font-family: var(--font-ui);
  }
  .new-btn:hover {
    border: 0;
    background: color-mix(in srgb, var(--color-accent) 88%, #fff);
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
    display: flex;
    flex-direction: column;
    padding: 18px 20px;
  }
  .weekrow {
    display: grid;
    grid-template-columns: repeat(7, 1fr);
    margin-bottom: 6px;
  }
  .weekday {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.1em;
    color: var(--color-muted);
    padding: 0 4px 4px;
  }
  .grid {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-columns: repeat(7, 1fr);
    grid-auto-rows: 1fr;
    gap: 1px;
    background: var(--color-border);
    border: var(--border-weight) solid var(--color-border);
  }
  .cell {
    background: var(--color-surface);
    padding: 7px 8px;
    overflow: hidden;
    cursor: pointer;
    border: 0;
    text-align: left;
    display: block;
  }
  .cell.out {
    opacity: 0.35;
    cursor: default;
  }
  .cell.today {
    box-shadow: inset 0 0 0 1.5px var(--color-accent);
  }
  .cell:focus-visible {
    outline: 1px solid var(--color-accent);
    outline-offset: -2px;
  }
  .cell-head {
    display: flex;
    justify-content: space-between;
    align-items: center;
  }
  .cell-num {
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-dim);
  }
  .cell.today .cell-num {
    font-weight: 600;
    color: var(--color-accent);
  }
  .chip-ev {
    margin-top: 4px;
    padding: 3px 5px;
    background: color-mix(in srgb, var(--color-accent) 16%, transparent);
    border-left: 2px solid var(--color-accent);
    overflow: hidden;
  }
  .chip-time {
    font-family: var(--font-mono);
    font-size: 8px;
    color: var(--color-accent);
  }
  .chip-title {
    font-size: 10px;
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
  .side-empty {
    padding: 16px;
    font-family: var(--font-mono);
    font-size: 10px;
    line-height: 1.6;
    color: var(--color-muted);
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
    font-size: 20px;
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
  .backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.6);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 50;
    padding: 24px;
    backdrop-filter: blur(2px);
  }
  .modal {
    width: 520px;
    max-width: 100%;
    max-height: 88vh;
    overflow-y: auto;
    overflow-x: hidden;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    box-shadow: 0 30px 80px rgba(0, 0, 0, 0.6);
  }
  .modal-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 16px 20px;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .modal-title {
    font-size: 15px;
    font-weight: 600;
    color: var(--color-text);
  }
  .modal-x {
    background: none;
    border: 0;
    color: var(--color-muted);
    font-size: 18px;
    line-height: 1;
    height: auto;
    padding: 0 4px;
  }
  .modal-x:hover {
    border: 0;
    color: var(--color-text);
  }
  .modal-body {
    padding: 20px;
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
  .modal-foot {
    display: flex;
    justify-content: flex-end;
    gap: 10px;
    padding: 16px 20px;
    border-top: var(--border-weight) solid var(--color-border);
  }
  .btn {
    height: auto;
    padding: 9px 18px;
    font-size: 12px;
    background: none;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    font-family: var(--font-ui);
  }
  .btn:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .btn.primary {
    padding: 9px 22px;
    font-weight: 600;
    border: 0;
    background: var(--color-accent);
    color: var(--color-accent-ink);
  }
  .btn.primary:hover {
    border: 0;
    background: color-mix(in srgb, var(--color-accent) 88%, #fff);
  }
</style>
