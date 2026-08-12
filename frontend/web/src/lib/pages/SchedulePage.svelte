<script lang="ts">
  import type { ScheduleEntryInfo } from "$lib/api/bridge";
  import CollectionDialog, { type DialogSpec } from "$lib/dialogs/CollectionDialog.svelte";
  import SessionDetail from "$lib/pages/history/SessionDetail.svelte";
  import CalendarMonth from "$lib/pages/schedule/CalendarMonth.svelte";
  import CalendarTimeGrid from "$lib/pages/schedule/CalendarTimeGrid.svelte";
  import EntryModal from "$lib/pages/schedule/EntryModal.svelte";
  import {
    addDays,
    armPhase,
    atMinute,
    destinationConflicts,
    linkedEntryIds,
    LIVE_STATES,
    MS_MIN,
    plannedItems,
    sameTimeOnDay,
    sessionItems,
    startOfDay,
    startOfWeek,
    type ArmPhase,
    type CalendarItem,
    type Conflict,
  } from "$lib/pages/schedule/layout";
  import { destinationIdentityStore } from "$lib/stores/destinationIdentityStore.svelte";
  import { scheduleStore } from "$lib/stores/scheduleStore.svelte";
  import { sessionsStore } from "$lib/stores/sessionsStore.svelte";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import PageShell from "$lib/ui/PageShell.svelte";
  import Segmented, { type SegmentedOption } from "$lib/ui/Segmented.svelte";

  // One timeline for what is planned and what actually ran. History and Upcoming
  // used to be two tabs beside the calendar showing the same weeks twice; a
  // recorded session is the same broadcast as the entry that planned it, at the
  // point where the plan became a fact, so it belongs in the same grid.
  //
  // This page owns state and navigation only. The three views are presentational
  // and the geometry lives in layout.ts, so none of the three can invent its own
  // idea of where a block goes.

  type View = "month" | "week" | "day";
  const VIEW_OPTIONS: SegmentedOption[] = [
    { label: "Month", value: "month" },
    { label: "Week", value: "week" },
    { label: "Day", value: "day" },
  ];

  const MONTH_NAMES = [
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
  ];

  const DEFAULT_NEW_MIN = 60;
  /** Where a create lands when the day was named but not the hour. */
  const DEFAULT_NEW_HOUR = 19;

  scheduleStore.start();
  sessionsStore.start();
  destinationIdentityStore.start();

  let view = $state<View>("week");
  let now = $state(Date.now());
  /** Local midnight the current view is anchored on. */
  let anchor = $state(startOfDay(Date.now()));

  // The countdown is computed here from starts_at rather than pushed: the host
  // emits only on state transitions, deliberately, so a per-second tick is the
  // client's job. One second while something is armed, coarse otherwise -- the
  // now line does not need better than half a minute.
  const armedNow = $derived(scheduleStore.entries.some((e) => e.state === "armed"));
  $effect(() => {
    const period = armedNow ? 1000 : 30_000;
    const id = setInterval(() => (now = Date.now()), period);
    return () => clearInterval(id);
  });

  // A drag commits through schedule.update and is reconciled by schedule.changed.
  // Until that round trip lands the block renders where it was dropped, so it does
  // not snap back to its old slot and then jump forward again.
  let pending = $state<Map<string, { startsAt: number; durationMin: number }>>(new Map());

  const entries = $derived.by<ScheduleEntryInfo[]>(() =>
    scheduleStore.entries.map((e) => {
      const p = pending.get(e.id);
      return p ? { ...e, startsAt: p.startsAt, durationMin: p.durationMin } : e;
    }),
  );

  const entryById = $derived(new Map(entries.map((e) => [e.id, e])));
  const items = $derived.by<CalendarItem[]>(() => {
    const sessions = sessionsStore.sessions;
    return [
      ...sessionItems(sessions, now, entryById),
      ...plannedItems(entries, now, linkedEntryIds(sessions)),
    ];
  });

  const conflicts = $derived(destinationConflicts(entries, now));
  function conflictOf(item: CalendarItem): Conflict | null {
    return item.entryId ? (conflicts.get(item.entryId) ?? null) : null;
  }
  function phaseOf(item: CalendarItem): ArmPhase {
    return armPhase(item, now);
  }

  const weekDays = $derived(
    Array.from({ length: 7 }, (_, i) => addDays(startOfWeek(anchor), i)),
  );
  const dayDays = $derived([anchor]);
  const anchorDate = $derived(new Date(anchor));

  const rangeLabel = $derived.by(() => {
    if (view === "month") {
      return `${MONTH_NAMES[anchorDate.getMonth()]} ${anchorDate.getFullYear()}`;
    }
    if (view === "day") {
      return anchorDate.toLocaleDateString(undefined, {
        weekday: "long",
        day: "numeric",
        month: "long",
        year: "numeric",
      });
    }
    const first = new Date(weekDays[0]);
    const last = new Date(weekDays[6]);
    const opts: Intl.DateTimeFormatOptions = { day: "numeric", month: "short" };
    return `${first.toLocaleDateString(undefined, opts)} – ${last.toLocaleDateString(undefined, opts)} ${last.getFullYear()}`;
  });

  const plannedCount = $derived(
    entries.filter((e) => LIVE_STATES.includes(e.state) && e.startsAt >= now).length,
  );

  function step(direction: number): void {
    if (view === "month") {
      const d = anchorDate;
      anchor = new Date(d.getFullYear(), d.getMonth() + direction, 1).getTime();
    } else {
      anchor = addDays(anchor, direction * (view === "week" ? 7 : 1));
    }
  }

  function goToday(): void {
    anchor = startOfDay(Date.now());
  }

  function goTo(startsAt: number): void {
    anchor = startOfDay(startsAt);
  }

  // --- modal / detail -----------------------------------------------------------

  interface Draft {
    entry: ScheduleEntryInfo | null;
    start: number;
    durationMin: number;
  }
  let draft = $state<Draft | null>(null);
  let openSessionId = $state<string | null>(null);

  function openCreate(start: number, durationMin: number): void {
    draft = { entry: null, start, durationMin };
  }

  // A day names no hour, so the evening slot is assumed -- rolled to the next whole
  // hour when that has already gone by, since a form pre-filled with a time that
  // has passed is a plan nothing can act on.
  function openDay(dayStart: number): void {
    const evening = atMinute(dayStart, DEFAULT_NEW_HOUR * 60);
    // Rounded on the local clock: dividing the epoch by an hour rounds to a UTC
    // hour, which is not a whole hour anywhere on a half-hour offset.
    const now = Date.now();
    const nextHour = atMinute(startOfDay(now), (new Date(now).getHours() + 1) * 60);
    openCreate(Math.max(evening, nextHour), DEFAULT_NEW_MIN);
  }

  function openItem(item: CalendarItem): void {
    if (item.kind === "session") {
      openSessionId = item.sessionId;
      return;
    }
    const entry = item.entryId ? entryById.get(item.entryId) : undefined;
    if (!entry) {
      return;
    }
    // A settled entry cannot be written -- the bridge refuses it -- so opening a
    // form that can only fail would be a worse answer than saying so.
    if (!LIVE_STATES.includes(entry.state)) {
      showToast(`That stream is ${entry.state} and can no longer be edited.`, entry.title);
      return;
    }
    draft = { entry, start: entry.startsAt, durationMin: entry.durationMin };
  }

  function showDay(dayStart: number): void {
    anchor = dayStart;
    view = "day";
  }

  function reject(reason: string): void {
    showToast(reason, reason);
  }

  // --- writes -------------------------------------------------------------------

  async function commit(item: CalendarItem, startsAt: number, durationMin: number): Promise<void> {
    const entry = item.entryId ? entryById.get(item.entryId) : undefined;
    if (!entry) {
      return;
    }
    const id = entry.id;
    pending = new Map(pending).set(id, { startsAt, durationMin });
    try {
      await scheduleStore.update(id, {
        startsAt,
        title: entry.title,
        durationMin,
        announce: entry.announce,
        autoStart: entry.autoStart,
        destinations: entry.destinations,
      });
      // Awaited even though schedule.changed will refresh anyway: `pending` is
      // cleared in the finally below, and clearing it before the new list has
      // landed drops the block back to its old slot for a frame.
      await scheduleStore.refresh();
    } catch (e) {
      const msg = (e as Error).message;
      showToast("Could not reschedule: " + msg, msg);
    } finally {
      const next = new Map(pending);
      next.delete(id);
      pending = next;
    }
  }

  function moveToDay(item: CalendarItem, dayStart: number): void {
    // The month grid has no hours, so the time of day is carried over unchanged.
    void commit(item, sameTimeOnDay(dayStart, item.start), Math.round((item.end - item.start) / MS_MIN));
  }

  async function cancelCountdown(item: CalendarItem): Promise<void> {
    if (!item.entryId) {
      return;
    }
    try {
      await scheduleStore.cancelCountdown(item.entryId);
    } catch (e) {
      // Surfaced rather than swallowed: a cancel that silently failed leaves the
      // user believing an unattended broadcast was stopped when it was not.
      const msg = (e as Error).message;
      showToast("Could not cancel the countdown: " + msg, msg);
    }
  }

  let dialog = $state<DialogSpec | null>(null);

  // Row-scoped in-flight guard, keyed by entry id like `pending` above, so a
  // double-click on the same chip cannot fire schedule.startNow twice.
  let goingLive = $state<Set<string>>(new Set());
  function goingLiveOf(item: CalendarItem): boolean {
    return item.entryId !== null && goingLive.has(item.entryId);
  }

  async function goLive(item: CalendarItem): Promise<void> {
    const id = item.entryId;
    if (!id || goingLive.has(id)) {
      return;
    }
    goingLive = new Set(goingLive).add(id);
    try {
      await scheduleStore.startNow(id);
    } catch (e) {
      // The host's message is already written for a streamer (already live,
      // already finished, no destination can go live) -- passed through as-is
      // rather than prefixed, the same way a drag rejection is.
      reject((e as Error).message);
    } finally {
      const next = new Set(goingLive);
      next.delete(id);
      goingLive = next;
    }
  }

  function confirmGoLive(item: CalendarItem): void {
    dialog = {
      kind: "confirm",
      title: "Go Live",
      // "switches your destinations to", not "to its enabled destinations": starting an
      // entry is what enables them, and it disables anything currently armed that the
      // entry does not name. Saying "enabled" reads as though nothing is about to change.
      message: `Go live now with "${item.title}"? This switches your destinations to the ones this entry names and starts broadcasting immediately.`,
      confirmLabel: "Go Live",
      onCommit: () => void goLive(item),
    };
  }

  const storesLoaded = $derived(scheduleStore.loaded && sessionsStore.loaded);
  const nothingAtAll = $derived(storesLoaded && items.length === 0);
</script>

<PageShell title="Schedule" sub="Planned streams and what actually ran">
  {#snippet actions()}
    {#if scheduleStore.error}
      <span class="shell-note" title={scheduleStore.error}>scheduling unavailable</span>
    {:else if sessionsStore.error}
      <span class="shell-note" title={sessionsStore.error}>history unavailable</span>
    {/if}
    <div class="nav">
      <button class="nav-btn" title="Previous" aria-label="Previous" onclick={() => step(-1)}>
        <Icon name="caret-left" size={14} />
      </button>
      <button class="today-btn" onclick={goToday}>Today</button>
      <button class="nav-btn" title="Next" aria-label="Next" onclick={() => step(1)}>
        <Icon name="caret-right" size={14} />
      </button>
      <span class="range">{rangeLabel} · {plannedCount} planned</span>
    </div>
    <Segmented options={VIEW_OPTIONS} value={view} onChange={(v) => (view = v as View)} />
    <button class="new-btn" onclick={() => openDay(startOfDay(Date.now()))}>
      <Icon name="plus" size={13} /> New Stream
    </button>
  {/snippet}

  <div class="body">
    {#if view === "month"}
      <CalendarMonth
        year={anchorDate.getFullYear()}
        month={anchorDate.getMonth()}
        {items}
        {now}
        {conflictOf}
        {phaseOf}
        onOpen={openItem}
        onCancel={cancelCountdown}
        onGoLive={confirmGoLive}
        {goingLiveOf}
        onCreate={openDay}
        onMoveDay={moveToDay}
        onShowDay={showDay}
        onReject={reject}
      />
    {:else}
      <CalendarTimeGrid
        days={view === "week" ? weekDays : dayDays}
        {items}
        {now}
        {conflictOf}
        {phaseOf}
        onOpen={openItem}
        onCancel={cancelCountdown}
        onGoLive={confirmGoLive}
        {goingLiveOf}
        onCreate={openCreate}
        onCommit={commit}
        onReject={reject}
      />
    {/if}

    {#if nothingAtAll}
      <!-- Over the grid, not instead of it: the two ways to create a stream are a
           drag on that grid and the button above it, and hiding the grid would
           hide one of them. -->
      <div class="watermark">
        <EmptyState
          title="Nothing scheduled yet"
          sub="Drag across the calendar to plan a stream, or use New Stream."
        >
          {#snippet icon()}
            <Icon name="film" size={22} />
          {/snippet}
        </EmptyState>
      </div>
    {/if}
  </div>
</PageShell>

{#if draft}
  <EntryModal
    entry={draft.entry}
    initialStart={draft.start}
    initialDurationMin={draft.durationMin}
    others={entries.filter((e) => e.id !== draft?.entry?.id)}
    onClose={() => (draft = null)}
    onSaved={goTo}
  />
{/if}

{#if openSessionId}
  <SessionDetail id={openSessionId} onClose={() => (openSessionId = null)} />
{/if}

{#if dialog}
  <CollectionDialog {...dialog} onClose={() => (dialog = null)} />
{/if}

<style>
  .nav {
    display: flex;
    align-items: center;
    gap: 6px;
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
  .today-btn {
    height: 24px;
    padding: 0 10px;
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: var(--letter-spacing);
    text-transform: uppercase;
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
  }
  .today-btn:hover {
    color: var(--color-accent);
    border-color: var(--color-accent);
  }
  .range {
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-dim);
    margin-left: 4px;
    white-space: nowrap;
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

  .body {
    position: relative;
    flex: 1;
    min-height: 0;
    display: flex;
  }
  .watermark {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    pointer-events: none;
  }
  .watermark :global(.card) {
    background: var(--color-surface);
  }
</style>
