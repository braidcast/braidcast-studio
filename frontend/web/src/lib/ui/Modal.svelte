<script lang="ts" module>
  // One footer cell. The bar takes actions, not markup: there is no snippet to write
  // a <button> into, so a dialog cannot put a control in the footer that this
  // component does not style. That is the whole point of the shape -- the previous
  // contract was three class names (`.accent`, `.ghost`, `.btn`) that the footer
  // matched with `:global()` but did not own, and two of the three had no shared
  // definition at all -- outside a modal each meant whatever the file it was written
  // in happened to define, or nothing.
  export interface ModalAction {
    label: string;
    onclick: () => void;
    disabled?: boolean;
  }

  // The weights live on the secondary type alone. `cancel` and `confirm` render fixed
  // cells, so a `tone` on either would be accepted and never applied; keeping it off
  // their type turns that into a compile error. The check is TypeScript's excess-
  // property rule, so it fires on the object literals every call site writes and not
  // on a pre-typed variable assigned in.
  export interface ModalSecondaryAction extends ModalAction {
    /** `strong` lifts a secondary above the quiet default; `danger` marks a destructive one. */
    tone?: "strong" | "danger";
  }
</script>

<script lang="ts">
  import type { Snippet } from "svelte";
  import IconButton from "$lib/ui/IconButton.svelte";
  import { pushEsc, popEsc, isTopEsc } from "$lib/utils/escStack";
  import { suspendPreview } from "$lib/stores/previewGate.svelte";

  // Shared modal shell: backdrop + surface panel + mono micro-label head + body,
  // with an optional footer. Replaces the hand-rolled per-dialog copies; a caller
  // owns its body content and describes its footer as actions (and any preview-gate
  // suspension it needs). Esc and the header close button always close; a backdrop
  // click never does (closeOnBackdrop defaults off so a stray click can't discard
  // work). A caller may opt back in with closeOnBackdrop={true}.
  //
  // Footer cells render left to right as actions, cancel, confirm. `confirm` is the
  // only accent cell there is, so a dialog cannot show two things that read as the
  // primary; a dialog with nothing to commit passes only `cancel`.
  interface Props {
    title: string;
    onClose: () => void;
    /** Panel width in px (clamped to the viewport). */
    width?: number;
    /** CSS max-height for the panel (default 86vh). */
    maxHeight?: string;
    closeOnBackdrop?: boolean;
    /** Drag the header to reposition the panel (kept within the viewport). Off by
     * default so every other modal keeps its fixed centered placement. */
    draggable?: boolean;
    /** Called on every drag step. A caller hosting a native overlay needs it: an
     * overlay HWND sits above CEF and does not follow the panel's CSS transform, so
     * only an explicit re-measure keeps it on the region it covers. */
    onMove?: () => void;
    /** Lay the body out as a flex column, so a child asking for `flex: 1 1 auto;
     * min-height: 0` fills the height the panel actually has instead of growing past
     * it and making the body scroll. Off by default: it stops sibling margins
     * collapsing, and blockifies inline children (an unwidthed button in the body
     * would stretch to full width). */
    fillBody?: boolean;
    /** Header-embedded controls (e.g. a segmented switch), right of the title. */
    headExtra?: Snippet;
    /** Standing text pinned to the footer's left edge (why an action is withheld). */
    note?: string;
    /** Secondary actions, quietest weight, left of `cancel`. */
    actions?: ModalSecondaryAction[];
    /** Abandon without committing. Omit where the dialog has nothing to abandon. */
    cancel?: ModalAction;
    /** The one action that ends the dialog affirmatively; the only accent cell. */
    confirm?: ModalAction;
    children: Snippet;
  }
  let {
    title,
    onClose,
    width = 520,
    maxHeight = "86vh",
    closeOnBackdrop = false,
    draggable = false,
    onMove,
    fillBody = false,
    headExtra,
    note,
    actions,
    cancel,
    confirm,
    children,
  }: Props = $props();

  const hasFooter = $derived(
    Boolean(note) || (actions?.length ?? 0) > 0 || cancel !== undefined || confirm !== undefined,
  );

  // With no confirm there is no filled cell, so nothing in a row of otherwise equal
  // quiet cells says which one ends the dialog. See `.foot-cell.terminal`.
  const terminalCancel = $derived(cancel !== undefined && confirm === undefined);

  // Header pointer-drag repositioning (opt-in). The offset is applied as a direct
  // transform on the panel (synchronous, so clamping can read the live rect); it is
  // never reset on close since the panel remounts per open.
  let modalEl = $state<HTMLDivElement | undefined>();
  let dragging = false;
  let curX = 0;
  let curY = 0;
  let lastX = 0;
  let lastY = 0;

  function onHeadPointerDown(e: PointerEvent) {
    // Never start a drag from an interactive control in the header (close button,
    // headExtra switches) — those must keep click behavior.
    if (!draggable || (e.target as HTMLElement).closest("button")) {
      return;
    }
    dragging = true;
    lastX = e.clientX;
    lastY = e.clientY;
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    e.preventDefault();
  }
  function onHeadPointerMove(e: PointerEvent) {
    if (!dragging || !modalEl) {
      return;
    }
    let nx = curX + (e.clientX - lastX);
    let ny = curY + (e.clientY - lastY);
    lastX = e.clientX;
    lastY = e.clientY;
    // Clamp so the panel stays fully on screen. baseLeft/baseTop = the untranslated
    // origin (rect minus the transform currently applied).
    const r = modalEl.getBoundingClientRect();
    const m = 8;
    const baseLeft = r.left - curX;
    const baseTop = r.top - curY;
    const minX = m - baseLeft;
    const maxX = window.innerWidth - m - r.width - baseLeft;
    const minY = m - baseTop;
    const maxY = window.innerHeight - m - r.height - baseTop;
    nx = Math.min(Math.max(nx, minX), Math.max(minX, maxX));
    ny = Math.min(Math.max(ny, minY), Math.max(minY, maxY));
    curX = nx;
    curY = ny;
    modalEl.style.transform = `translate(${curX}px, ${curY}px)`;
    onMove?.();
  }
  function onHeadPointerUp(e: PointerEvent) {
    dragging = false;
    try {
      (e.currentTarget as HTMLElement).releasePointerCapture(e.pointerId);
    } catch {
      // capture may already be gone; ignore
    }
  }

  // The native OBS preview is a child HWND z-ordered above CEF, so any modal would
  // paint behind it. Suspend the preview for this modal's lifetime; the effect
  // cleanup (the release fn suspendPreview returns) re-asserts it on destroy.
  $effect(() => suspendPreview());

  // Gate Escape so a menu (or nested modal) stacked above this one closes first;
  // only the topmost Escape owner acts.
  let escToken: symbol | undefined;
  $effect(() => {
    escToken = pushEsc("modal");
    return () => {
      if (escToken) popEsc(escToken);
      escToken = undefined;
    };
  });

  // Focus management: move focus into the panel on open, trap Tab within it, and
  // return focus to the trigger on close so AT/keyboard users can't reach the
  // obscured page behind the modal.
  const FOCUSABLE =
    'a[href],area[href],input:not([disabled]),select:not([disabled]),' +
    'textarea:not([disabled]),button:not([disabled]),iframe,object,embed,' +
    '[contenteditable],audio[controls],video[controls],[tabindex]:not([tabindex="-1"])';

  function focusables(): HTMLElement[] {
    if (!modalEl) {
      return [];
    }
    return Array.from(modalEl.querySelectorAll<HTMLElement>(FOCUSABLE)).filter(
      (el) => el.offsetParent !== null || getComputedStyle(el).position === "fixed",
    );
  }

  let triggerEl: HTMLElement | null = null;
  $effect(() => {
    triggerEl = document.activeElement as HTMLElement | null;
    (focusables()[0] ?? modalEl)?.focus();
    return () => {
      if (triggerEl && document.contains(triggerEl)) {
        triggerEl.focus();
      }
    };
  });

  function onKeydown(e: KeyboardEvent) {
    if (!escToken || !isTopEsc(escToken)) {
      return;
    }
    if (e.key === "Escape") {
      onClose();
      return;
    }
    if (e.key === "Tab") {
      const items = focusables();
      if (items.length === 0) {
        e.preventDefault();
        modalEl?.focus();
        return;
      }
      const first = items[0];
      const last = items[items.length - 1];
      const active = document.activeElement as HTMLElement | null;
      const outside = !modalEl?.contains(active);
      if (e.shiftKey && (active === first || outside)) {
        e.preventDefault();
        last.focus();
      } else if (!e.shiftKey && (active === last || outside)) {
        e.preventDefault();
        first.focus();
      }
    }
  }
</script>

<svelte:window onkeydown={onKeydown} />

<div
  class="modal-backdrop"
  role="presentation"
  onclick={(e) => {
    if (closeOnBackdrop && e.target === e.currentTarget) onClose();
  }}
>
  <div
    class="modal"
    role="dialog"
    aria-modal="true"
    aria-label={title}
    tabindex="-1"
    bind:this={modalEl}
    style:width={`min(${width}px, 100%)`}
    style:max-height={maxHeight}
  >
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <header
      class="modal-head"
      class:draggable
      onpointerdown={onHeadPointerDown}
      onpointermove={onHeadPointerMove}
      onpointerup={onHeadPointerUp}
    >
      <h3>{title}</h3>
      {#if headExtra}
        <div class="head-extra">{@render headExtra()}</div>
      {/if}
      <IconButton icon="x" size={22} height={20} title="Close" aria-label="Close" onclick={onClose} />
    </header>

    <div class="modal-body" class:fill={fillBody}>{@render children()}</div>

    {#if hasFooter}
      <footer class="modal-foot">
        {#if note}<span class="foot-note">{note}</span>{/if}
        {#each actions ?? [] as a}
          <button
            type="button"
            class="foot-cell"
            class:strong={a.tone === "strong"}
            class:danger={a.tone === "danger"}
            disabled={a.disabled}
            onclick={a.onclick}
          >
            {a.label}
          </button>
        {/each}
        {#if cancel}
          <button
            type="button"
            class="foot-cell cancel"
            class:terminal={terminalCancel}
            disabled={cancel.disabled}
            onclick={cancel.onclick}
          >
            {cancel.label}
          </button>
        {/if}
        {#if confirm}
          <button
            type="button"
            class="foot-cell accent"
            disabled={confirm.disabled}
            onclick={confirm.onclick}
          >
            {confirm.label}
          </button>
        {/if}
      </footer>
    {/if}
  </div>
</div>

<style>
  .modal-backdrop {
    position: fixed;
    inset: 0;
    background: var(--backdrop);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 100;
    padding: 24px;
  }
  .modal {
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    box-shadow: var(--shadow-modal);
    display: flex;
    flex-direction: column;
    min-height: 0;
    font-family: var(--font-ui);
  }
  .modal-head {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 8px 11px;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .modal-head.draggable {
    cursor: move;
    touch-action: none;
  }
  .modal-head h3 {
    margin: 0 auto 0 0;
    font-family: var(--font-mono);
    font-size: 11px;
    font-weight: 600;
    letter-spacing: var(--letter-spacing);
    text-transform: uppercase;
    color: var(--color-text);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .head-extra {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 8px;
    min-width: 0;
  }
  .modal-body {
    flex: 1 1 auto;
    min-height: 0;
    padding: 16px 14px;
    overflow: auto;
  }
  /* Opt-in (fillBody): lets one child bound itself to the panel's real height rather
     than overflowing it. Not the default -- it changes margin collapsing and inline
     child sizing for bodies written against block flow. */
  .modal-body.fill {
    display: flex;
    flex-direction: column;
  }
  /* Footer action bar. Every cell fills the bar full-height and sits flush to the
     corner — the same edge-to-edge block as the Studio GO LIVE button — while the
     note stays centered with left padding. min-height gives the bar a definite
     height for the stretch. */
  .modal-foot {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 8px;
    min-height: 42px;
    padding: 0 0 0 12px;
    border-top: var(--border-weight) solid var(--color-border);
  }
  .foot-note {
    flex: 1 1 auto;
    font-size: 11px;
    color: var(--color-muted);
  }
  /* Bar geometry, shared by every cell: cancels the global `button` rule's padding
     and height (app.css:183-191) once, in the component that owns the bar.
     `height: auto` with `align-self: stretch` is what makes a cell as tall as the
     bar instead of --control-height. */
  .foot-cell {
    align-self: stretch;
    height: auto;
    margin: 0;
    padding: 0 22px;
    border: 0;
    border-left: var(--border-weight) solid var(--color-border);
  }
  /* Everything but the confirm cell. Scoping the quiet look behind :not(.accent)
     leaves `button.accent` (app.css:202-209) to paint the one filled block: a scoped
     class outranks that element-plus-class global, so a rule that applied to every
     cell would have had to restate the fill here to undo itself. Mono and dimmed, so
     the accent cell stays dominant. --color-dim measures 5.20:1 at worst (Industrial)
     against --color-surface. */
  .foot-cell:not(.accent) {
    background: var(--color-surface);
    font-family: var(--font-mono);
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-dim);
  }
  /* A footer with no confirm has no filled cell, so nothing separates the dismiss
     from the secondaries beside it. The mark is the cell's ground, not its ink: the
     accent cell has to stay the only thing that reads as primary, and a brightened
     Cancel would compete with it in the dialogs that have both. --color-dim measures
     4.81:1 against --color-surface-2 at worst (Industrial), so the label still clears
     4.5:1 on the raised cell. */
  .foot-cell.terminal {
    background: var(--color-surface-2);
  }
  /* Fourth weight, for a secondary that must sit above the quiet default without
     borrowing the accent the confirm cell keeps to itself. */
  .foot-cell.strong {
    color: var(--color-text);
    font-weight: 600;
  }
  .foot-cell.danger {
    color: var(--color-live);
  }
  .foot-cell:not(.accent, .strong, .danger):hover:not(:disabled) {
    color: var(--color-text);
  }
  /* These two already carry a lifted ink, so their hover moves the cell's divider
     rather than its ink or its ground: recoloring a Delete's label would read as it
     having stopped being destructive, and raising the ground under --color-live
     costs contrast the label cannot spare (4.25:1 on --color-surface against 3.84:1
     on --color-surface-2, Slate). An edge is outside the label, so it changes
     nothing the label is measured against. */
  .foot-cell.strong:hover:not(:disabled) {
    border-left-color: var(--color-text);
  }
  .foot-cell.danger:hover:not(:disabled) {
    border-left-color: var(--color-live);
  }
</style>
