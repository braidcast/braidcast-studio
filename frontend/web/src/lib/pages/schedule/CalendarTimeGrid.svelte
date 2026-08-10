<script lang="ts">
  import EventChip from "./EventChip.svelte";
  import {
    atMinute,
    daySegments,
    HOUR_PX,
    MIN_BLOCK_PX,
    minutesInto,
    MINUTES_PER_DAY,
    MS_MIN,
    sameDay,
    SNAP_MIN,
    type ArmPhase,
    type CalendarItem,
    type Conflict,
  } from "./layout";

  // The hour grid behind both Week and Day. One component for both because they
  // differ only in how many columns they carry -- a separate Day view is how the
  // two drift into disagreeing about snapping, the now line, or what the past is.
  interface Props {
    /** Local midnight per column, in display order. */
    days: number[];
    items: CalendarItem[];
    now: number;
    conflictOf: (item: CalendarItem) => Conflict | null;
    phaseOf: (item: CalendarItem) => ArmPhase;
    onOpen: (item: CalendarItem) => void;
    onCancel: (item: CalendarItem) => void;
    onCreate: (startMs: number, durationMin: number) => void;
    onCommit: (item: CalendarItem, startMs: number, durationMin: number) => void;
    /** A gesture that would land in the past; the page says why. */
    onReject: (reason: string) => void;
  }
  let { days, items, now, conflictOf, phaseOf, onOpen, onCancel, onCreate, onCommit, onReject }: Props =
    $props();

  const HOURS = Array.from({ length: 24 }, (_, h) => h);
  const DEFAULT_NEW_MIN = 60;
  /** Below this the thumbnail crowds out the text it is meant to illustrate. */
  const THUMB_MIN_PX = 92;
  /** How far the pointer must travel before a press counts as a drag. Without it a
   * trackpad's own jitter turns every click on a chip into a reschedule. */
  const DRAG_THRESHOLD_PX = 4;

  const DOW = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];

  interface DragState {
    mode: "create" | "move" | "resize";
    key: string;
    item: CalendarItem | null;
    /** Pointer minute minus the item's start minute, so a grabbed block keeps the
     * point it was grabbed by instead of jumping its top edge to the cursor.
     * Negative for a block grabbed on the day it continues into, which is why the
     * minute it produces must stay signed all the way through atMinute. */
    grabOffsetMin: number;
    /** Where the press landed, for the threshold above. */
    originX: number;
    originY: number;
    startMs: number;
    endMs: number;
    moved: boolean;
  }
  let drag = $state<DragState | null>(null);
  let colsEl = $state<HTMLDivElement | undefined>();
  let scrollEl = $state<HTMLDivElement | undefined>();

  // A pointer capture ends with a synthetic click on the captured subtree, so a
  // block that was just dragged would also open its modal. Cleared on the macrotask
  // after the click has been dispatched.
  let justDragged = false;
  function openGuarded(item: CalendarItem): void {
    if (!justDragged) {
      onOpen(item);
    }
  }

  // Roving tabindex over the hour slots: one tab stop for the whole grid, arrows
  // move within it. 24 x 7 individually tabbable cells would bury the chips.
  let focusCol = $state(0);
  let focusHour = $state(new Date().getHours());

  // Open on the current hour rather than at midnight -- the top of a day grid is
  // eight hours of nothing for most people. Read off a plain const, not the ticking
  // `now`, or every countdown tick would yank the scroll position back.
  const initialHour = new Date().getHours();
  $effect(() => {
    const el = scrollEl;
    if (el) {
      el.scrollTop = Math.max(0, initialHour * HOUR_PX - HOUR_PX);
    }
  });

  function locate(e: PointerEvent): { dayIndex: number; minutes: number } | null {
    if (!colsEl) {
      return null;
    }
    const rect = colsEl.getBoundingClientRect();
    const colWidth = rect.width / days.length;
    const idx = Math.min(Math.max(Math.floor((e.clientX - rect.left) / colWidth), 0), days.length - 1);
    // Bounded to the column it resolved to. This is the one place a gesture is
    // pinned to a day: a pointer dragged past the top or bottom edge is still
    // pointing at THAT day. atMinute deliberately does not clamp, so an offset
    // applied to this can still cross midnight when a move calls for it.
    const minutes = Math.min(Math.max(((e.clientY - rect.top) / HOUR_PX) * 60, 0), MINUTES_PER_DAY);
    return { dayIndex: idx, minutes };
  }

  function pastThreshold(e: PointerEvent, d: DragState): boolean {
    const dx = e.clientX - d.originX;
    const dy = e.clientY - d.originY;
    return dx * dx + dy * dy >= DRAG_THRESHOLD_PX * DRAG_THRESHOLD_PX;
  }

  function snap(minutes: number): number {
    return Math.round(minutes / SNAP_MIN) * SNAP_MIN;
  }

  function capture(e: PointerEvent): void {
    colsEl?.setPointerCapture(e.pointerId);
  }

  function beginCreate(e: PointerEvent, dayIndex: number): void {
    if (e.button !== 0) {
      return;
    }
    const at = locate(e);
    if (!at) {
      return;
    }
    const start = atMinute(days[dayIndex], at.minutes);
    if (start < now) {
      // Reachable only in the slot the clock is currently inside: every earlier
      // one is a disabled button. Saying so beats a dead band that swallows the
      // gesture without explanation.
      onReject("That time has already passed.");
      return;
    }
    drag = {
      mode: "create",
      key: "",
      item: null,
      grabOffsetMin: 0,
      originX: e.clientX,
      originY: e.clientY,
      startMs: start,
      endMs: start + SNAP_MIN * MS_MIN,
      moved: false,
    };
    capture(e);
    e.preventDefault();
  }

  function beginItemDrag(e: PointerEvent, item: CalendarItem, mode: "move" | "resize"): void {
    if (e.button !== 0 || !item.editable) {
      return;
    }
    const at = locate(e);
    if (!at) {
      return;
    }
    drag = {
      mode,
      key: item.key,
      item,
      grabOffsetMin: at.minutes - minutesInto(days[at.dayIndex], item.start),
      originX: e.clientX,
      originY: e.clientY,
      startMs: item.start,
      endMs: item.end,
      moved: false,
    };
    capture(e);
    e.preventDefault();
    e.stopPropagation();
  }

  function onPointerMove(e: PointerEvent): void {
    const d = drag;
    if (!d) {
      return;
    }
    // Nothing moves until the press has travelled far enough to be a drag, so the
    // block does not shift a pixel under a click either.
    if (!d.moved && !pastThreshold(e, d)) {
      return;
    }
    const at = locate(e);
    if (!at) {
      return;
    }
    const dayStart = days[at.dayIndex];
    if (d.mode === "create") {
      const edge = atMinute(dayStart, at.minutes);
      const lo = Math.min(edge, d.startMs);
      const hi = Math.max(edge, d.startMs);
      drag = { ...d, startMs: lo, endMs: Math.max(hi, lo + SNAP_MIN * MS_MIN), moved: true };
    } else if (d.mode === "move") {
      const duration = d.endMs - d.startMs;
      const startMs = atMinute(dayStart, snap(at.minutes - d.grabOffsetMin));
      drag = { ...d, startMs, endMs: startMs + duration, moved: true };
    } else {
      const endMs = atMinute(dayStart, at.minutes);
      drag = { ...d, endMs: Math.max(endMs, d.startMs + SNAP_MIN * MS_MIN), moved: true };
    }
  }

  function releaseCapture(e: PointerEvent): void {
    try {
      colsEl?.releasePointerCapture(e.pointerId);
    } catch {
      // capture may already be gone; nothing here depends on it
    }
  }

  function onPointerUp(e: PointerEvent): void {
    const d = drag;
    drag = null;
    if (!d) {
      return;
    }
    releaseCapture(e);
    if (d.moved) {
      justDragged = true;
      setTimeout(() => (justDragged = false), 0);
    }
    const durationMin = Math.max(SNAP_MIN, Math.round((d.endMs - d.startMs) / MS_MIN));
    if (d.mode === "create") {
      onCreate(d.startMs, d.moved ? durationMin : DEFAULT_NEW_MIN);
      return;
    }
    if (!d.item) {
      return;
    }
    // Decided on the VALUES, not on whether the pointer twitched: a gesture that
    // ends where it began is a click even if it wandered, and a write that changes
    // nothing is still a write the runner reacts to.
    const wasMin = Math.round((d.item.end - d.item.start) / MS_MIN);
    if (d.startMs === d.item.start && durationMin === wasMin) {
      return;
    }
    if (d.startMs < now) {
      onReject("A stream cannot be moved into the past.");
      return;
    }
    onCommit(d.item, d.startMs, durationMin);
  }

  // A cancelled gesture is an abandoned one -- the OS took the pointer away (a
  // touch became a scroll, the window lost focus), which is not a drop and must
  // not commit anything.
  function onPointerCancel(e: PointerEvent): void {
    drag = null;
    releaseCapture(e);
  }

  // The dragged block renders at its dragged geometry, so the pointer is not
  // chasing a block that only moves once the bridge answers.
  const shown = $derived.by<CalendarItem[]>(() => {
    const d = drag;
    if (!d || d.mode === "create" || !d.item) {
      return items;
    }
    return items.map((i) => (i.key === d.key ? { ...i, start: d.startMs, end: d.endMs } : i));
  });

  const columns = $derived(
    days.map((dayStart) => ({ dayStart, segments: daySegments(shown, dayStart) })),
  );

  function slotLabel(dayStart: number, hour: number): string {
    const d = new Date(dayStart);
    return `Schedule ${DOW[(d.getDay() + 6) % 7]} ${d.getDate()} ${String(hour).padStart(2, "0")}:00`;
  }

  function slotIsPast(dayStart: number, hour: number): boolean {
    return dayStart + (hour + 1) * 60 * MS_MIN <= now;
  }

  // Past slots are disabled, and a disabled button cannot hold the grid's single
  // tab stop -- landing it there loses keyboard access to the whole grid. So the
  // roving position is resolved to the first schedulable slot whenever the
  // remembered one is not one.
  const tabSlot = $derived.by(() => {
    const col = Math.min(focusCol, days.length - 1);
    if (!slotIsPast(days[col], focusHour)) {
      return { col, hour: focusHour };
    }
    for (let c = 0; c < days.length; c++) {
      for (let h = 0; h < 24; h++) {
        if (!slotIsPast(days[c], h)) {
          return { col: c, hour: h };
        }
      }
    }
    return { col: days.length - 1, hour: 23 };
  });

  function focusSlot(col: number, hour: number): void {
    focusCol = col;
    focusHour = hour;
    const el = colsEl?.querySelector<HTMLElement>(`[data-slot="${col}-${hour}"]`);
    el?.focus();
    el?.scrollIntoView({ block: "nearest" });
  }

  function onSlotKeydown(e: KeyboardEvent, col: number, hour: number): void {
    switch (e.key) {
      case "ArrowDown":
        focusSlot(col, Math.min(23, hour + 1));
        break;
      case "ArrowUp":
        focusSlot(col, Math.max(0, hour - 1));
        break;
      case "ArrowRight":
        focusSlot(Math.min(days.length - 1, col + 1), hour);
        break;
      case "ArrowLeft":
        focusSlot(Math.max(0, col - 1), hour);
        break;
      case "Home":
        focusSlot(col, 0);
        break;
      case "End":
        focusSlot(col, 23);
        break;
      case "Enter":
      case " ":
        onCreate(days[col] + hour * 60 * MS_MIN, DEFAULT_NEW_MIN);
        break;
      default:
        return;
    }
    e.preventDefault();
  }

  function blockHeight(topMin: number, endMin: number): number {
    return Math.max(((endMin - topMin) / 60) * HOUR_PX, MIN_BLOCK_PX);
  }

  function blockStyle(topMin: number, endMin: number, col: number, cols: number): string {
    const width = 100 / cols;
    return (
      `top:${(topMin / 60) * HOUR_PX}px;height:${blockHeight(topMin, endMin)}px;` +
      `left:${col * width}%;width:calc(${width}% - 3px);`
    );
  }

  /** The planned block behind a session that ran against it -- the only way the
   * grid can say "this was meant to be two hours and took three". It takes the
   * same packed slot as its session, or it would span the whole column and sit
   * behind blocks it has nothing to do with. */
  function ghostStyle(item: CalendarItem, dayStart: number, col: number, cols: number): string {
    const top = Math.max(0, minutesInto(dayStart, item.plannedStart ?? 0));
    const end = Math.min(MINUTES_PER_DAY, minutesInto(dayStart, item.plannedEnd ?? 0));
    const width = 100 / cols;
    return (
      `top:${(top / 60) * HOUR_PX}px;height:${Math.max(((end - top) / 60) * HOUR_PX, 2)}px;` +
      `left:${col * width}%;width:calc(${width}% - 3px);`
    );
  }
</script>

<div class="tg">
  <div class="tg-scroll" bind:this={scrollEl}>
    <div class="tg-head">
      <div class="gutter-head"></div>
      {#each days as dayStart (dayStart)}
        {@const d = new Date(dayStart)}
        <div class="dayhead" class:today={sameDay(dayStart, now)}>
          <span class="dow">{DOW[(d.getDay() + 6) % 7]}</span>
          <span class="dnum">{d.getDate()}</span>
        </div>
      {/each}
    </div>

    <div class="tg-body" style="height: {24 * HOUR_PX}px">
      <div class="gutter">
        {#each HOURS as h (h)}
          <div class="hourlabel" style="height: {HOUR_PX}px">
            {#if h > 0}<span>{String(h).padStart(2, "0")}:00</span>{/if}
          </div>
        {/each}
      </div>

      <!-- svelte-ignore a11y_no_static_element_interactions -->
      <div
        class="cols"
        bind:this={colsEl}
        onpointermove={onPointerMove}
        onpointerup={onPointerUp}
        onpointercancel={onPointerUp}
      >
        {#each columns as column, ci (column.dayStart)}
          <div class="col">
            <!-- The immutable past, shaded rather than merely un-draggable: the
                 boundary is the same line the now indicator draws. -->
            {#if column.dayStart + MINUTES_PER_DAY * MS_MIN <= now}
              <div class="pastfill" style="top:0;height:{24 * HOUR_PX}px"></div>
            {:else if sameDay(column.dayStart, now)}
              <div
                class="pastfill"
                style="top:0;height:{(minutesInto(column.dayStart, now) / 60) * HOUR_PX}px"
              ></div>
            {/if}

            {#each HOURS as h (h)}
              <button
                class="slot"
                type="button"
                data-slot={ci + "-" + h}
                style="height: {HOUR_PX}px"
                aria-label={slotLabel(column.dayStart, h)}
                disabled={slotIsPast(column.dayStart, h)}
                tabindex={tabSlot.col === ci && tabSlot.hour === h ? 0 : -1}
                onpointerdown={(e) => beginCreate(e, ci)}
                onkeydown={(e) => onSlotKeydown(e, ci, h)}
                onfocus={() => {
                  focusCol = ci;
                  focusHour = h;
                }}
              ></button>
            {/each}

            <div class="events">
              {#each column.segments as seg (seg.key)}
                {#if seg.item.plannedStart !== null && seg.item.plannedEnd !== null}
                  <div
                    class="ghost"
                    style={ghostStyle(seg.item, column.dayStart, seg.col, seg.cols)}
                    aria-hidden="true"
                  >
                    <span class="ghost-tag">planned</span>
                  </div>
                {/if}
                <div class="block" style={blockStyle(seg.topMin, seg.endMin, seg.col, seg.cols)}>
                  <EventChip
                    item={seg.item}
                    variant="block"
                    {now}
                    conflict={conflictOf(seg.item)}
                    phase={phaseOf(seg.item)}
                    showThumb={blockHeight(seg.topMin, seg.endMin) >= THUMB_MIN_PX}
                    onOpen={openGuarded}
                    {onCancel}
                    onDragStart={beginItemDrag}
                    resizable
                  />
                </div>
              {/each}
            </div>

            {#if drag && drag.mode === "create" && sameDay(drag.startMs, column.dayStart)}
              <div
                class="ghost-new"
                style="top:{(minutesInto(column.dayStart, drag.startMs) / 60) *
                  HOUR_PX}px;height:{((drag.endMs - drag.startMs) / MS_MIN / 60) * HOUR_PX}px"
              ></div>
            {/if}

            {#if sameDay(column.dayStart, now)}
              <div class="nowline" style="top: {(minutesInto(column.dayStart, now) / 60) * HOUR_PX}px">
                <span class="nowdot"></span>
              </div>
            {/if}
          </div>
        {/each}
      </div>
    </div>
  </div>
</div>

<style>
  .tg {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
    background: var(--color-base);
  }
  .tg-scroll {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    overflow-x: hidden;
  }
  /* Sticky rather than a sibling above the scroll box: the two must share one width,
     and a scrollbar appearing on the body would otherwise shift the columns out from
     under their own headings. */
  .tg-head {
    position: sticky;
    top: 0;
    z-index: 5;
    display: flex;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .gutter-head {
    flex: 0 0 54px;
    border-right: var(--border-weight) solid var(--color-border);
  }
  .dayhead {
    flex: 1;
    min-width: 0;
    display: flex;
    align-items: baseline;
    gap: 6px;
    padding: 8px 10px;
    border-right: var(--border-weight) solid var(--color-border-2);
  }
  .dayhead:last-child {
    border-right: 0;
  }
  .dow {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .dnum {
    font-size: 15px;
    font-weight: 600;
    color: var(--color-dim);
  }
  .dayhead.today .dnum,
  .dayhead.today .dow {
    color: var(--color-accent);
  }

  .tg-body {
    display: flex;
    position: relative;
  }
  .gutter {
    flex: 0 0 54px;
    border-right: var(--border-weight) solid var(--color-border);
  }
  .hourlabel {
    position: relative;
    display: flex;
    justify-content: flex-end;
    padding-right: 8px;
  }
  .hourlabel span {
    position: relative;
    top: -6px;
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--color-muted);
  }

  .cols {
    flex: 1;
    min-width: 0;
    display: flex;
    touch-action: none;
  }
  .col {
    position: relative;
    flex: 1;
    min-width: 0;
    border-right: var(--border-weight) solid var(--color-border-2);
  }
  .col:last-child {
    border-right: 0;
  }
  .pastfill {
    position: absolute;
    left: 0;
    right: 0;
    background: color-mix(in srgb, var(--color-base) 55%, transparent);
    pointer-events: none;
    z-index: 1;
  }
  .slot {
    display: block;
    width: 100%;
    padding: 0;
    background: transparent;
    border: 0;
    border-bottom: var(--border-weight) solid var(--color-border-2);
    cursor: cell;
  }
  .slot:hover:not(:disabled) {
    background: color-mix(in srgb, var(--color-accent) 6%, transparent);
    border: 0;
    border-bottom: var(--border-weight) solid var(--color-border-2);
  }
  .slot:disabled {
    cursor: default;
  }
  .slot:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: -2px;
  }

  .events {
    position: absolute;
    inset: 0;
    pointer-events: none;
    z-index: 2;
  }
  .block {
    position: absolute;
    display: flex;
    pointer-events: auto;
  }
  .block :global(.chip) {
    flex: 1;
    min-width: 0;
  }
  .ghost {
    position: absolute;
    border: var(--border-weight) dashed var(--color-border);
    pointer-events: none;
  }
  .ghost-tag {
    position: absolute;
    right: 2px;
    top: 1px;
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.08em;
    color: var(--color-muted);
  }
  .ghost-new {
    position: absolute;
    left: 0;
    right: 3px;
    background: color-mix(in srgb, var(--color-accent) 22%, transparent);
    border-left: 2px solid var(--color-accent);
    pointer-events: none;
    z-index: 3;
  }

  .nowline {
    position: absolute;
    left: 0;
    right: 0;
    height: 0;
    border-top: 1px solid var(--color-live);
    pointer-events: none;
    z-index: 4;
  }
  .nowdot {
    position: absolute;
    left: -3px;
    top: -3px;
    width: 6px;
    height: 6px;
    background: var(--color-live);
  }
</style>
