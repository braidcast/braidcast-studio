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
  import Modal from "./Modal.svelte";

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

  // Modal owns Escape (always closes). Enter confirms from anywhere (window-level,
  // so confirm/alert kinds without a focused field still commit on Enter).
  function onKeydown(e: KeyboardEvent) {
    if (e.key === "Enter") {
      commit();
    }
  }
</script>

<svelte:window onkeydown={onKeydown} />

<Modal {title} {onClose} width={360}>
  {#if kind === "prompt"}
    <input class="field" bind:value aria-label={title} use:focusOnMount spellcheck="false" />
  {:else if message}
    <p class="msg">{message}</p>
  {/if}

  {#snippet footer()}
    {#if kind !== "alert"}
      <button class="ghost" onclick={onClose}>Cancel</button>
    {/if}
    <button class="accent" disabled={!valid} onclick={commit}>{confirmLabel}</button>
  {/snippet}
</Modal>

<style>
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
</style>
