<script lang="ts">
  import { destinationIdentityStore } from "$lib/stores/destinationIdentityStore.svelte";
  import {
    SCHEDULE_STATE_EDGE,
    SCHEDULE_STATE_LABEL,
    SESSION_END_LABEL,
    SESSION_END_STATE,
    STATE_COLOR_EXT,
    type EdgeState,
  } from "$lib/theme/stateColors";
  import Icon from "$lib/ui/Icon.svelte";
  import PlatformMark from "$lib/ui/PlatformMark.svelte";
  import Thumb from "$lib/ui/Thumb.svelte";
  import { thumbDataUri } from "$lib/utils/thumbCache";
  import {
    clockLabel,
    countdownLabel,
    formatDuration,
    overrunMin,
    type ArmPhase,
    type CalendarItem,
    type Conflict,
    MS_MIN,
  } from "./layout";

  // One entry on the timeline, in every view. A planned entry and a recorded
  // session are the same component deliberately: they are the same thing at two
  // points in its life, and two components is how the two stop looking alike.
  interface Props {
    item: CalendarItem;
    /** `block` is the absolutely-positioned Week/Day form; `compact` is the single
     * line a month cell has room for. */
    variant: "block" | "compact";
    /** Ticking clock, so the countdown is derived here rather than pushed. */
    now: number;
    conflict: Conflict | null;
    phase: ArmPhase;
    /** The block is tall enough to carry the session thumbnail. */
    showThumb?: boolean;
    onOpen: (item: CalendarItem) => void;
    onCancel: (item: CalendarItem) => void;
    /** Absent in views with no drag (or on an item nothing may move). */
    onDragStart?: (e: PointerEvent, item: CalendarItem, mode: "move" | "resize") => void;
    /** Week/Day only: the bottom edge resizes the planned duration. */
    resizable?: boolean;
  }
  let {
    item,
    variant,
    now,
    conflict,
    phase,
    showThumb = false,
    onOpen,
    onCancel,
    onDragStart,
    resizable = false,
  }: Props = $props();

  const edge = $derived<EdgeState>(
    item.kind === "session"
      ? item.running
        ? "live"
        : (SESSION_END_STATE[item.endReason ?? ""] ?? "off")
      : (SCHEDULE_STATE_EDGE[item.state ?? "planned"] ?? "off"),
  );

  const stateLabel = $derived(
    item.kind === "session"
      ? item.running
        ? "Live"
        : (SESSION_END_LABEL[item.endReason ?? ""] ?? "Ended")
      : (SCHEDULE_STATE_LABEL[item.state ?? "planned"] ?? "Planned"),
  );

  // What the chip prints where the state label goes. The countdown replaces it
  // only while it is running: "Armed" is still the state, and the T- figure is
  // the more urgent reading of the same fact.
  const badge = $derived(
    phase === "countdown"
      ? countdownLabel(item.start, now)
      : phase === "canceled"
        ? "Auto-start canceled"
        : phase === "armed"
          ? `Armed ${countdownLabel(item.start, now)}`
          : stateLabel,
  );

  const timeLabel = $derived(clockLabel(item.start));
  const rangeLabel = $derived(`${clockLabel(item.start)}–${clockLabel(item.end)}`);
  const overrun = $derived(overrunMin(item));
  const durationLabel = $derived(formatDuration(Math.round((item.end - item.start) / MS_MIN)));

  // Cancel belongs on the chip rather than inside a modal: auto-start's danger is
  // being unattended, so the stop has to be where the eye already is. Offered for
  // the whole armed window, not just the last minute -- the bridge refuses it in
  // any other state, which is the same condition these two phases stand for.
  const cancellable = $derived(phase === "armed" || phase === "countdown");

  const platforms = $derived(
    item.profileIds.map((id) => destinationIdentityStore.forProfile(id)?.platform ?? "rtmp"),
  );

  const conflictText = $derived(
    conflict
      ? `Destination conflict: shares a stream profile with ${conflict.others.join(", ")}. One profile cannot carry two concurrent outputs.`
      : "",
  );

  const accessibleName = $derived(
    [
      item.title,
      rangeLabel,
      badge,
      overrun > 0 ? `ran ${formatDuration(overrun)} over plan` : "",
      item.blockReason ? `Cannot go live: ${item.blockReason}` : "",
      conflictText,
    ]
      .filter((part) => part !== "")
      .join(" — "),
  );

  let dataUri = $state("");
  $effect(() => {
    const file = showThumb ? item.thumbFile : "";
    let cancelled = false;
    void thumbDataUri(file).then((uri) => {
      if (!cancelled) {
        dataUri = uri;
      }
    });
    return () => {
      cancelled = true;
    };
  });

  function onMainPointerDown(e: PointerEvent): void {
    if (onDragStart && item.editable) {
      onDragStart(e, item, "move");
    }
  }
  function onHandlePointerDown(e: PointerEvent): void {
    if (onDragStart && item.editable) {
      onDragStart(e, item, "resize");
    }
  }
</script>

<div
  class="chip {variant}"
  class:conflicted={conflict !== null}
  class:actual={item.kind === "session"}
  class:draggable={item.editable && onDragStart !== undefined}
  style="--edge: {STATE_COLOR_EXT[edge]}"
>
  <button
    class="main"
    type="button"
    title={accessibleName}
    aria-label={accessibleName}
    onpointerdown={onMainPointerDown}
    onclick={() => onOpen(item)}
  >
    {#if variant === "compact"}
      <span class="c-time">{timeLabel}</span>
      <span class="c-title">{item.title}</span>
      {#if conflict || item.blockReason}
        <span class="c-warn"><Icon name="warn" size={10} /></span>
      {/if}
      {#if phase === "countdown" || phase === "armed"}
        <span class="c-badge">{badge}</span>
      {/if}
    {:else}
      <span class="head">
        <span class="time">{rangeLabel}</span>
        <span class="state">{badge}</span>
      </span>
      <span class="title">{item.title}</span>
      {#if showThumb && dataUri}
        <span class="thumb"><Thumb src={dataUri} alt="" /></span>
      {/if}
      <span class="foot">
        {#if platforms.length > 0}
          <span class="marks">
            {#each platforms as p, i (item.profileIds[i])}
              <PlatformMark platform={p} size={11} />
            {/each}
          </span>
        {/if}
        <span class="dur">{durationLabel}</span>
        {#if overrun > 0}
          <span class="over">+{formatDuration(overrun)} over</span>
        {/if}
      </span>
      {#if item.blockReason}
        <span class="warn"><Icon name="warn" size={11} /> {item.blockReason}</span>
      {:else if conflict}
        <span class="warn"><Icon name="warn" size={11} /> Conflict</span>
      {/if}
    {/if}
  </button>

  {#if cancellable}
    <button
      class="cancel"
      type="button"
      title="Cancel this occurrence's auto-start"
      aria-label="Cancel auto-start for {item.title}"
      onclick={() => onCancel(item)}
    >
      {#if variant === "compact"}<Icon name="x" size={9} />{:else}Cancel{/if}
    </button>
  {/if}

  {#if resizable && item.editable && onDragStart}
    <!-- Pointer-only: the keyboard path to a duration change is the modal, which
         edits it as a number instead of as a gesture. -->
    <div
      class="rz"
      role="presentation"
      aria-hidden="true"
      onpointerdown={onHandlePointerDown}
    ></div>
  {/if}
</div>

<style>
  .chip {
    position: relative;
    display: flex;
    min-width: 0;
    min-height: 0;
    background: color-mix(in srgb, var(--edge) 16%, var(--color-surface));
    border-left: 2px solid var(--edge);
    overflow: hidden;
  }
  /* A run that already happened is a record, not a plan: it recedes so the editable
     future reads as the active layer of the same grid. */
  .chip.actual {
    background: color-mix(in srgb, var(--edge) 9%, var(--color-base));
  }
  /* The conflict cannot ride the left edge -- that edge already means output state.
     A dashed outline plus the written word carries it instead. */
  .chip.conflicted {
    outline: var(--border-weight) dashed var(--color-live);
    outline-offset: -1px;
  }
  /* The host owns the block's geometry (a component root cannot take an inline
     style), so this only says how the content stacks inside it. */
  .chip.block {
    flex-direction: column;
  }
  .chip.compact {
    width: 100%;
    align-items: center;
  }

  .main {
    flex: 1;
    min-width: 0;
    min-height: 0;
    height: auto;
    display: flex;
    padding: 3px 6px;
    background: transparent;
    border: 0;
    color: inherit;
    font: inherit;
    text-align: left;
    overflow: hidden;
  }
  .main:hover {
    background: color-mix(in srgb, var(--color-text) 6%, transparent);
    border: 0;
  }
  .main:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: -2px;
  }
  .chip.draggable .main {
    cursor: grab;
    touch-action: none;
  }

  /* ---- compact (month cell) ---- */
  .compact .main {
    align-items: baseline;
    gap: 5px;
    padding: 2px 5px;
  }
  .c-time {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--edge);
  }
  .c-title {
    flex: 1;
    min-width: 0;
    font-size: 11px;
    color: var(--color-text);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .c-warn {
    flex: 0 0 auto;
    display: inline-flex;
    color: var(--color-live);
  }
  .c-badge {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.06em;
    color: var(--meter-yellow);
  }

  /* ---- block (week / day) ---- */
  .block .main {
    flex-direction: column;
    gap: 2px;
  }
  .head {
    display: flex;
    align-items: baseline;
    gap: 6px;
    min-width: 0;
  }
  .time {
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--edge);
    white-space: nowrap;
  }
  .state {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    color: var(--color-muted);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .title {
    font-size: 11px;
    line-height: 1.25;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .thumb {
    display: block;
    width: 100%;
    max-width: 96px;
    margin-top: 2px;
  }
  .foot {
    display: flex;
    align-items: center;
    gap: 6px;
    margin-top: auto;
    padding-top: 2px;
    min-width: 0;
  }
  .marks {
    display: flex;
    align-items: center;
    gap: 4px;
    flex: 0 0 auto;
  }
  .dur {
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--color-muted);
  }
  .over {
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--meter-yellow);
    white-space: nowrap;
  }
  .warn {
    display: flex;
    align-items: center;
    gap: 4px;
    min-width: 0;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    color: var(--color-live);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .cancel {
    flex: 0 0 auto;
    align-self: flex-start;
    display: inline-flex;
    align-items: center;
    height: auto;
    margin: 2px;
    padding: 2px 6px;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    color: var(--color-live);
    background: transparent;
    border: var(--border-weight) solid color-mix(in srgb, var(--color-live) 55%, transparent);
  }
  .cancel:hover {
    color: var(--color-accent-ink);
    background: var(--color-live);
    border-color: var(--color-live);
  }
  .block .cancel {
    position: absolute;
    right: 2px;
    bottom: 2px;
  }

  .rz {
    position: absolute;
    left: 0;
    right: 0;
    bottom: 0;
    height: 6px;
    cursor: ns-resize;
    touch-action: none;
  }
  .rz:hover {
    background: color-mix(in srgb, var(--edge) 60%, transparent);
  }
</style>
