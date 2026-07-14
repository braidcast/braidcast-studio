<script lang="ts">
  import type { Snippet } from "svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import { pushEsc, popEsc, isTopEsc } from "$lib/utils/escStack";
  import { suspendPreview } from "$lib/stores/previewGate.svelte";

  // Shared modal shell: backdrop + surface panel + mono micro-label head + body,
  // with an optional footer. Replaces the hand-rolled per-dialog copies; a caller
  // owns only its body/footer content (and any preview-gate suspension it needs).
  // Esc and the header close button always close; a backdrop click never does
  // (closeOnBackdrop defaults off so a stray click can't discard work). A caller
  // may opt back in with closeOnBackdrop={true}.
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
    /** Header-embedded controls (e.g. a segmented switch), right of the title. */
    headExtra?: Snippet;
    footer?: Snippet;
    children: Snippet;
  }
  let {
    title,
    onClose,
    width = 520,
    maxHeight = "86vh",
    closeOnBackdrop = false,
    draggable = false,
    headExtra,
    footer,
    children,
  }: Props = $props();

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
    escToken = pushEsc();
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
      <button class="close" title="Close" aria-label="Close" onclick={onClose}>
        <Icon name="x" size={13} />
      </button>
    </header>

    <div class="modal-body">{@render children()}</div>

    {#if footer}
      <footer class="modal-foot">{@render footer()}</footer>
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
  .close {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    justify-content: center;
    width: 22px;
    height: 20px;
    padding: 0;
    background: none;
    border: 0;
    color: var(--color-muted);
  }
  .close:hover {
    color: var(--color-text);
    border: 0;
  }
  .modal-body {
    flex: 1 1 auto;
    min-height: 0;
    padding: 16px 14px;
    overflow: auto;
  }
  /* Footer action bar. The primary button (.accent, or a non-ghost .btn) fills the
     bar full-height and sits flush to the corner — the same edge-to-edge block as
     the Studio GO LIVE button — while notes and secondary buttons stay centered with
     left padding. min-height gives the bar a definite height for the stretch. */
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
  .modal-foot :global(.accent),
  .modal-foot :global(.btn:not(.ghost)) {
    align-self: stretch;
    height: auto;
    margin: 0;
    padding: 0 22px;
    border: 0;
    border-left: var(--border-weight) solid var(--color-border);
  }
  /* Secondary/cancel action: a de-emphasized ghost that keeps its natural height
     (never stretched) so the primary block stays the dominant edge-to-edge cell.
     Centralized here so every modal footer's Cancel reads like the GO LIVE one. */
  .modal-foot :global(.ghost) {
    font-family: var(--font-mono);
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: uppercase;
    color: var(--color-dim);
    background: var(--color-surface);
  }
  .modal-foot :global(.ghost:hover) {
    color: var(--color-text);
    border-color: var(--color-border);
  }
</style>
