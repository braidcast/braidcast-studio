<script lang="ts">
  import { tick } from "svelte";
  import { obs } from "./bridge";

  interface Props {
    onClose: () => void;
  }
  let { onClose }: Props = $props();

  // The native preview overlay is suspended by the opener for this dialog's whole
  // lifetime, so it never occludes the modal.

  let path = $state("");
  let contents = $state("");
  let loaded = $state(false);
  let error = $state<string | null>(null);
  let copied = $state(false);
  let pre = $state<HTMLPreElement | null>(null);

  async function load() {
    try {
      const res = await obs.call("log.getCurrent");
      path = res.path;
      contents = res.contents;
      error = null;
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
      // Show the newest output first, matching "view current log" expectation.
      await tick();
      if (pre) {
        pre.scrollTop = pre.scrollHeight;
      }
    }
  }

  $effect(() => {
    void load();
  });

  async function copy() {
    try {
      await navigator.clipboard.writeText(contents);
      copied = true;
      setTimeout(() => (copied = false), 1500);
    } catch (e) {
      error = (e as Error).message;
    }
  }

  async function openFolder() {
    if (!path) return;
    try {
      await obs.call("shell.revealPath", { path });
    } catch (e) {
      error = (e as Error).message;
    }
  }

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
    if (e.target === e.currentTarget) onClose();
  }}
>
  <div class="modal" role="dialog" aria-modal="true" aria-label="Current Log">
    <header class="modal-head">
      <h3>Current Log</h3>
      <button class="icon close" title="Close" onclick={onClose}>✕</button>
    </header>

    <div class="modal-body">
      {#if error}<p class="error">{error}</p>{/if}
      {#if path}<p class="path" title={path}>{path}</p>{/if}

      {#if !loaded}
        <p class="dim">Loading…</p>
      {:else}
        <pre bind:this={pre} class="log">{contents}</pre>
      {/if}
    </div>

    <footer class="modal-foot">
      <button class="btn ghost" onclick={() => void copy()}>{copied ? "Copied" : "Copy"}</button>
      <button class="btn ghost" onclick={() => void load()}>Refresh</button>
      <button class="btn ghost" disabled={!path} onclick={() => void openFolder()}>Open Folder</button>
      <button class="btn" onclick={onClose}>Close</button>
    </footer>
  </div>
</div>

<style>
  .modal-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.55);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 100;
    padding: 24px;
  }
  .modal {
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    width: min(900px, 100%);
    max-height: 86vh;
    display: flex;
    flex-direction: column;
    box-shadow: 0 18px 48px rgba(0, 0, 0, 0.5);
    font-family: var(--font-ui);
  }
  .modal-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 8px 11px;
    background: var(--color-surface);
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .modal-head h3 {
    margin: 0;
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-text);
    font-weight: 600;
  }
  .modal-body {
    padding: 12px;
    overflow: hidden;
    display: flex;
    flex-direction: column;
    min-height: 0;
    flex: 1;
  }
  .modal-foot {
    display: flex;
    justify-content: flex-end;
    gap: 6px;
    padding: 8px 11px;
    border-top: var(--border-weight) solid var(--color-border);
  }

  .path {
    margin: 0 0 8px;
    font-size: 10px;
    color: var(--color-muted);
    font-family: var(--font-mono, monospace);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .log {
    margin: 0;
    flex: 1;
    min-height: 0;
    overflow: auto;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-mono, monospace);
    font-size: 11px;
    line-height: 1.45;
    padding: 8px;
    white-space: pre;
    tab-size: 4;
  }

  .btn {
    height: auto;
    padding: 5px 10px;
    font-family: var(--font-ui);
    font-size: 11px;
    border: var(--border-weight) solid var(--color-border);
    background: transparent;
    color: var(--color-text);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    white-space: nowrap;
  }
  .btn:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .btn.ghost {
    background: none;
  }

  .icon {
    background: none;
    border: none;
    color: var(--color-muted);
    cursor: pointer;
    padding: 2px 4px;
    font-size: 13px;
    line-height: 1;
    height: auto;
  }
  .icon:hover {
    color: var(--color-text);
  }
  .dim {
    color: var(--color-muted);
    margin: 0;
    padding: 4px 0;
    font-size: 11px;
  }
  .error {
    color: var(--color-live);
    margin: 0 0 8px;
    font-size: 11px;
  }
</style>
