<script lang="ts">
  import type { Snippet } from "svelte";
  import Icon from "./dock/Icon.svelte";

  // Shared modal shell: backdrop + surface panel + mono micro-label head + body,
  // with an optional footer. Replaces the hand-rolled per-dialog copies; a caller
  // owns only its body/footer content (and any preview-gate suspension it needs).
  // Esc always closes; callers with unsaved state pass closeOnBackdrop={false} to
  // keep a stray backdrop click from discarding work.
  interface Props {
    title: string;
    onClose: () => void;
    /** Panel width in px (clamped to the viewport). */
    width?: number;
    /** CSS max-height for the panel (default 86vh). */
    maxHeight?: string;
    closeOnBackdrop?: boolean;
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
    closeOnBackdrop = true,
    headExtra,
    footer,
    children,
  }: Props = $props();

  function onKeydown(e: KeyboardEvent) {
    if (e.key === "Escape") {
      onClose();
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
    style:width={`min(${width}px, 100%)`}
    style:max-height={maxHeight}
  >
    <header class="modal-head">
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
  .modal-foot {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 8px;
    padding: 8px 11px;
    border-top: var(--border-weight) solid var(--color-border);
  }
</style>
