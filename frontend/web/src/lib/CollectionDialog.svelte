<script lang="ts" module>
  // A minimal modal reused by the Scene Collection menu. Three kinds:
  //   prompt  -> single-line text entry (New / Rename); commits the trimmed value
  //   confirm -> a yes/no question (Delete); commits ""
  //   alert   -> a single-button error notice (surfaces a rejected bridge call)
  // Styled with the shell's --color-* tokens, zero border-radius (project rule);
  // it deliberately does NOT use the rounded --bg-raised set.
  export type DialogKind = "prompt" | "confirm" | "alert";

  export interface DialogSpec {
    kind: DialogKind;
    title: string;
    /** confirm/alert body text. */
    message?: string;
    /** prompt initial input value. */
    initial?: string;
    /** Affirmative-button label (Create / Rename / Delete / OK). */
    confirmLabel?: string;
    /** prompt -> receives the trimmed value; confirm -> receives "". */
    onCommit?: (value: string) => void;
  }
</script>

<script lang="ts">
  import { untrack } from "svelte";

  let {
    kind,
    title,
    message = "",
    initial = "",
    confirmLabel = "OK",
    onCommit,
    onClose,
  }: DialogSpec & { onClose: () => void } = $props();

  // Snapshot the prop once; this dialog mounts fresh per open (Rename seeds the
  // current name), so the input is uncontrolled after that. untrack marks the
  // one-time read intentional, silencing svelte-check's state_referenced_locally.
  let value = $state(untrack(() => initial));

  const valid = $derived(kind !== "prompt" || value.trim().length > 0);

  function focusOnMount(node: HTMLInputElement) {
    node.focus();
    node.select();
  }

  function commit() {
    if (!valid) {
      return;
    }
    onCommit?.(kind === "prompt" ? value.trim() : "");
    onClose();
  }

  function onKeydown(e: KeyboardEvent) {
    if (e.key === "Escape") {
      onClose();
    } else if (e.key === "Enter") {
      commit();
    }
  }
</script>

<svelte:window onkeydown={onKeydown} />

<div
  class="backdrop"
  role="presentation"
  onclick={(e) => {
    if (e.target === e.currentTarget) {
      onClose();
    }
  }}
>
  <div class="dialog" role="dialog" aria-modal="true" aria-label={title}>
    <header class="head">{title}</header>
    <div class="body">
      {#if kind === "prompt"}
        <input class="field" bind:value aria-label={title} use:focusOnMount spellcheck="false" />
      {:else if message}
        <p class="msg">{message}</p>
      {/if}
    </div>
    <footer class="actions">
      {#if kind !== "alert"}
        <button class="btn" onclick={onClose}>Cancel</button>
      {/if}
      <button class="btn primary" disabled={!valid} onclick={commit}>{confirmLabel}</button>
    </footer>
  </div>
</div>

<style>
  .backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.55);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 200;
    padding: 24px;
  }
  .dialog {
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    width: min(360px, 100%);
    display: flex;
    flex-direction: column;
    box-shadow: 0 8px 24px rgba(0, 0, 0, 0.6);
  }
  .head {
    padding: 8px 12px;
    border-bottom: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  .body {
    padding: 12px;
  }
  .msg {
    margin: 0;
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    line-height: 1.5;
  }
  .field {
    width: 100%;
    box-sizing: border-box;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-accent);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 4px 6px;
  }
  .field:focus {
    outline: none;
  }
  .actions {
    display: flex;
    justify-content: flex-end;
    gap: 6px;
    padding: 0 12px 12px;
  }
  .btn {
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    padding: 4px 12px;
  }
  .btn:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .btn.primary {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .btn.primary:disabled {
    color: var(--color-muted);
    border-color: var(--color-border);
    cursor: default;
  }
</style>
