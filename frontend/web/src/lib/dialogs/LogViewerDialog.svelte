<script lang="ts">
  import { tick } from "svelte";
  import Modal from "$lib/ui/Modal.svelte";
  import { obs } from "$lib/api/bridge";

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
</script>

<Modal title="Current Log" {onClose} width={900}>
  {#if error}<p class="error">{error}</p>{/if}
  {#if path}<p class="path" title={path}>{path}</p>{/if}

  {#if !loaded}
    <p class="dim">Loading…</p>
  {:else}
    <pre bind:this={pre} class="log selectable">{contents}</pre>
  {/if}

  {#snippet footer()}
    <button class="ghost" onclick={() => void copy()}>{copied ? "Copied" : "Copy"}</button>
    <button class="ghost" onclick={() => void load()}>Refresh</button>
    <button class="ghost" disabled={!path} onclick={() => void openFolder()}>Open Folder</button>
    <button class="btn" onclick={onClose}>Close</button>
  {/snippet}
</Modal>

<style>
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
    max-height: 70vh;
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
