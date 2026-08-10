<script lang="ts">
  import EventChip from "./EventChip.svelte";
  import {
    addDays,
    monthCells,
    sameTimeOnDay,
    startOfDay,
    type ArmPhase,
    type CalendarItem,
    type Conflict,
    type MonthCell,
  } from "./layout";

  // The month grid. Duration has no room to be a gesture here, so a drag moves an
  // entry across days and keeps its time -- the length is edited in the modal.
  interface Props {
    year: number;
    /** 0-based, as Date reports it. */
    month: number;
    items: CalendarItem[];
    now: number;
    conflictOf: (item: CalendarItem) => Conflict | null;
    phaseOf: (item: CalendarItem) => ArmPhase;
    onOpen: (item: CalendarItem) => void;
    onCancel: (item: CalendarItem) => void;
    onCreate: (dayStart: number) => void;
    onMoveDay: (item: CalendarItem, dayStart: number) => void;
    /** "+N more" hands the day to the Day view rather than growing the cell. */
    onShowDay: (dayStart: number) => void;
    onReject: (reason: string) => void;
  }
  let {
    year,
    month,
    items,
    now,
    conflictOf,
    phaseOf,
    onOpen,
    onCancel,
    onCreate,
    onMoveDay,
    onShowDay,
    onReject,
  }: Props = $props();

  const WEEKDAYS = ["MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"];
  /** Beyond this a cell is a list, not a calendar square. */
  const MAX_CHIPS = 3;

  const cells = $derived(monthCells(year, month, now));

  // An item lands in every day it touches: a stream that crosses midnight is on
  // both days, which is what a month grid is expected to say. The walk is bounded
  // by the visible grid rather than by the item, so a session whose end is far away
  // (a crashed row still reading as running) cannot turn this into a long loop.
  const byDay = $derived.by<Map<number, CalendarItem[]>>(() => {
    const map = new Map<number, CalendarItem[]>();
    if (cells.length === 0) {
      return map;
    }
    const gridStart = cells[0].dayStart;
    const gridEnd = addDays(cells[cells.length - 1].dayStart, 1);
    for (const item of items) {
      if (item.end <= gridStart || item.start >= gridEnd) {
        continue;
      }
      const last = Math.min(startOfDay(Math.max(item.start, item.end - 1)), cells[cells.length - 1].dayStart);
      for (let day = Math.max(startOfDay(item.start), gridStart); day <= last; day = addDays(day, 1)) {
        const list = map.get(day);
        if (list) {
          list.push(item);
        } else {
          map.set(day, [item]);
        }
      }
    }
    for (const list of map.values()) {
      list.sort((a, b) => a.start - b.start);
    }
    return map;
  });

  let gridEl = $state<HTMLDivElement | undefined>();
  let dragKey = $state<string | null>(null);
  let dragItem: CalendarItem | null = null;
  let dragDay = $state<number | null>(null);

  // A pointer capture ends with a synthetic click on the captured subtree, so the
  // cell a chip was dropped on would also fire its create button, and the chip
  // itself would open its modal. Cleared on the macrotask after the click lands.
  let justDragged = false;
  function openGuarded(item: CalendarItem): void {
    if (!justDragged) {
      onOpen(item);
    }
  }
  function createGuarded(dayStart: number): void {
    if (!justDragged) {
      onCreate(dayStart);
    }
  }

  function dayUnderPointer(e: PointerEvent): number | null {
    if (!gridEl) {
      return null;
    }
    for (const el of gridEl.querySelectorAll<HTMLElement>("[data-day]")) {
      const r = el.getBoundingClientRect();
      if (e.clientX >= r.left && e.clientX < r.right && e.clientY >= r.top && e.clientY < r.bottom) {
        return Number(el.dataset.day);
      }
    }
    return null;
  }

  function beginDrag(e: PointerEvent, item: CalendarItem): void {
    if (e.button !== 0 || !item.editable) {
      return;
    }
    dragKey = item.key;
    dragItem = item;
    dragDay = startOfDay(item.start);
    gridEl?.setPointerCapture(e.pointerId);
    e.preventDefault();
    e.stopPropagation();
  }

  function onPointerMove(e: PointerEvent): void {
    if (dragKey === null) {
      return;
    }
    const day = dayUnderPointer(e);
    if (day !== null) {
      dragDay = day;
    }
  }

  /** Drop the gesture and hand the capture back, reporting what was being dragged so
   * a drop can still act on it once the state is cleared. */
  function endDrag(e: PointerEvent): { item: CalendarItem | null; day: number | null } {
    const held = { item: dragItem, day: dragDay };
    dragKey = null;
    dragItem = null;
    dragDay = null;
    try {
      gridEl?.releasePointerCapture(e.pointerId);
    } catch {
      // capture may already be gone; the drop still stands
    }
    return held;
  }

  function onPointerUp(e: PointerEvent): void {
    const { item, day } = endDrag(e);
    if (!item || day === null || day === startOfDay(item.start)) {
      return;
    }
    justDragged = true;
    setTimeout(() => (justDragged = false), 0);
    if (sameTimeOnDay(day, item.start) < now) {
      onReject("A stream cannot be moved into the past.");
      return;
    }
    onMoveDay(item, day);
  }

  // A cancelled gesture is an abandoned one -- the OS took the pointer away (a touch
  // became a scroll, the window lost focus), which is not a drop and must not move
  // the entry to whichever day the pointer was last over.
  function onPointerCancel(e: PointerEvent): void {
    endDrag(e);
  }

  function cellLabel(cell: MonthCell): string {
    return `Schedule ${new Date(cell.dayStart).toLocaleDateString(undefined, {
      weekday: "long",
      day: "numeric",
      month: "long",
    })}`;
  }

  // Roving tabindex across the month: one tab stop, arrows walk days and weeks.
  // Past cells are disabled, and a disabled button cannot hold the tab stop, so it
  // falls through to the first day something can still be planned in.
  let focusDay = $state<number | null>(null);
  const tabDay = $derived(
    focusDay !== null && cells.some((c) => c.dayStart === focusDay && !c.isPast)
      ? focusDay
      : (cells.find((c) => c.isToday)?.dayStart ??
        cells.find((c) => !c.isPast)?.dayStart ??
        null),
  );

  function focusCell(dayStart: number): void {
    focusDay = dayStart;
    gridEl?.querySelector<HTMLElement>(`[data-cellbtn="${dayStart}"]`)?.focus();
  }

  function onCellKeydown(e: KeyboardEvent, cell: MonthCell): void {
    const step = { ArrowLeft: -1, ArrowRight: 1, ArrowUp: -7, ArrowDown: 7 }[e.key];
    if (step !== undefined) {
      const target = addDays(cell.dayStart, step);
      if (cells.some((c) => c.dayStart === target)) {
        focusCell(target);
      }
      e.preventDefault();
      return;
    }
    if (e.key === "Enter" || e.key === " ") {
      onCreate(cell.dayStart);
      e.preventDefault();
    }
  }
</script>

<div class="month">
  <div class="weekrow">
    {#each WEEKDAYS as w (w)}
      <div class="weekday">{w}</div>
    {/each}
  </div>

  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <div class="grid" bind:this={gridEl} onpointermove={onPointerMove} onpointerup={onPointerUp} onpointercancel={onPointerCancel}>
    {#each cells as cell (cell.key)}
      {@const dayItems = byDay.get(cell.dayStart) ?? []}
      <div
        class="cell"
        class:out={!cell.inMonth}
        class:today={cell.isToday}
        class:past={cell.isPast}
        class:droptarget={dragKey !== null && dragDay === cell.dayStart}
        data-day={cell.dayStart}
      >
        <button
          class="cell-bg"
          type="button"
          data-cellbtn={cell.dayStart}
          aria-label={cellLabel(cell)}
          disabled={cell.isPast}
          tabindex={tabDay === cell.dayStart ? 0 : -1}
          onclick={() => createGuarded(cell.dayStart)}
          onkeydown={(e) => onCellKeydown(e, cell)}
          onfocus={() => (focusDay = cell.dayStart)}
        ></button>

        <div class="cell-head" aria-hidden="true">
          <span class="num">{cell.dayNum}</span>
        </div>

        <div class="cell-events">
          {#each dayItems.slice(0, MAX_CHIPS) as item (item.key)}
            <EventChip
              {item}
              variant="compact"
              {now}
              conflict={conflictOf(item)}
              phase={phaseOf(item)}
              onOpen={openGuarded}
              {onCancel}
              onDragStart={beginDrag}
            />
          {/each}
          {#if dayItems.length > MAX_CHIPS}
            <button class="more" type="button" onclick={() => onShowDay(cell.dayStart)}>
              +{dayItems.length - MAX_CHIPS} more
            </button>
          {/if}
        </div>
      </div>
    {/each}
  </div>
</div>

<style>
  .month {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
    overflow: auto;
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
    color: var(--color-muted);
    padding: 9px 12px;
  }
  .grid {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-columns: repeat(7, 1fr);
    grid-auto-rows: minmax(104px, 1fr);
    gap: var(--border-weight);
    background: var(--color-border);
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .cell {
    position: relative;
    min-height: 0;
    display: flex;
    flex-direction: column;
    gap: 3px;
    padding: 4px 5px 6px;
    background: var(--color-surface);
    overflow: hidden;
  }
  .cell.out {
    background: var(--color-base);
  }
  /* A past day is legible but visibly closed: nothing can be planned into it. */
  .cell.past {
    background: color-mix(in srgb, var(--color-base) 60%, var(--color-surface));
  }
  .cell.today {
    background: color-mix(in srgb, var(--color-accent) 7%, var(--color-surface));
  }
  .cell.droptarget {
    box-shadow: inset 0 0 0 1px var(--color-accent);
  }
  /* The create affordance is the cell's whole background, so the chips can be real
     buttons on top of it instead of divs inside one. */
  .cell-bg {
    position: absolute;
    inset: 0;
    padding: 0;
    background: transparent;
    border: 0;
    cursor: cell;
  }
  .cell-bg:hover:not(:disabled) {
    background: color-mix(in srgb, var(--color-text) 4%, transparent);
    border: 0;
  }
  .cell-bg:disabled {
    cursor: default;
    opacity: 1;
  }
  .cell-bg:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: -2px;
  }
  .cell-head {
    position: relative;
    display: flex;
    pointer-events: none;
  }
  .num {
    font-family: var(--font-mono);
    font-size: 12px;
    color: var(--color-muted);
  }
  .cell.today .num {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 22px;
    height: 22px;
    font-weight: 600;
    color: var(--color-accent-ink);
    background: var(--color-accent);
  }
  .cell-events {
    position: relative;
    display: flex;
    flex-direction: column;
    gap: 2px;
    min-height: 0;
    pointer-events: none;
  }
  .cell-events :global(.chip) {
    pointer-events: auto;
  }
  .more {
    align-self: flex-start;
    pointer-events: auto;
    height: auto;
    padding: 1px 5px;
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--color-muted);
    background: transparent;
    border: 0;
  }
  .more:hover {
    color: var(--color-accent);
    border: 0;
  }
</style>
