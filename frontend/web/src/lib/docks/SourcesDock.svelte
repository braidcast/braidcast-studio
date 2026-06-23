<script lang="ts">
  import { onMount } from "svelte";
  import { obs, type SceneItem, type ReorderDirection } from "../bridge";
  import { defaultCanvas } from "./defaultCanvasStore.svelte";
  import AddSourceModal from "../AddSourceModal.svelte";
  import PropertyForm from "../properties/PropertyForm.svelte";
  import { suspendPreview } from "../previewGate.svelte";

  let {}: Record<string, unknown> = $props();

  onMount(() => defaultCanvas.start());

  // The default canvas's current scene drives this list (global channel-0 path).
  const currentScene = $derived(defaultCanvas.current);

  let items = $state<SceneItem[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);
  let selectedItemId = $state<number | null>(null);
  let propsForSource = $state<string | null>(null);
  let adding = $state(false);

  function report(e: unknown) {
    error = (e as Error).message;
  }

  function openProperties(item: SceneItem) {
    if (item.source) {
      propsForSource = item.source;
    }
  }

  function selectItem(item: SceneItem) {
    selectedItemId = item.id;
    void obs.call("preview.select", { scene: currentScene, id: item.id });
  }

  // The properties modal overlaps the preview; suspend the native overlay while open.
  $effect(() => {
    if (propsForSource) {
      return suspendPreview();
    }
  });

  // Reflect preview-driven selection (click in the overlay) back into the list.
  $effect(() => {
    return obs.on("sceneItem.selected", (p) => {
      if (!p.scene || p.scene === currentScene) {
        selectedItemId = p.id;
      }
    });
  });

  async function load() {
    if (!currentScene) {
      items = [];
      loaded = true;
      return;
    }
    error = null;
    try {
      items = await obs.call("sceneItems.list", { scene: currentScene });
      if (selectedItemId !== null && !items.some((i) => i.id === selectedItemId)) {
        selectedItemId = null;
      }
    } catch (e) {
      report(e);
    } finally {
      loaded = true;
    }
  }

  // Reload whenever the current scene changes.
  $effect(() => {
    void currentScene;
    void load();
  });

  // Refresh on item mutations targeting the global path (canvas=null) for our scene.
  $effect(() => {
    return obs.on("sceneItems.changed", (p) => {
      if (p.canvas == null && (!p.scene || p.scene === currentScene)) {
        void load();
      }
    });
  });

  function onSourceCreated(created: { id: number; source: string }) {
    adding = false;
    selectedItemId = created.id;
    propsForSource = created.source;
    void load();
  }

  async function toggleVisible(item: SceneItem) {
    try {
      await obs.call("sceneItems.setVisible", { scene: currentScene, id: item.id, visible: !item.visible });
    } catch (e) {
      report(e);
    }
  }

  async function toggleLocked(item: SceneItem) {
    try {
      await obs.call("sceneItems.setLocked", { scene: currentScene, id: item.id, locked: !item.locked });
    } catch (e) {
      report(e);
    }
  }

  async function reorder(item: SceneItem, direction: ReorderDirection) {
    try {
      await obs.call("sceneItems.reorder", { scene: currentScene, id: item.id, direction });
    } catch (e) {
      report(e);
    }
  }

  async function remove(item: SceneItem) {
    try {
      await obs.call("sceneItems.remove", { scene: currentScene, id: item.id });
    } catch (e) {
      report(e);
    }
  }
</script>

<div class="dock">
  <div class="toolbar">
    <button class="add" title="Add source" disabled={!currentScene} onclick={() => (adding = true)}>＋</button>
  </div>

  {#if error}
    <p class="msg err">{error}</p>
  {:else if !currentScene}
    <p class="msg">No scene selected</p>
  {:else if !loaded}
    <p class="msg">Loading…</p>
  {:else if items.length === 0}
    <p class="msg">No sources</p>
  {:else}
    <ul class="list">
      {#each items as item, idx (item.id)}
        <li class="row" class:sel={item.id === selectedItemId} class:hidden-src={!item.visible}>
          <button class="icon" title={item.visible ? "Hide" : "Show"} onclick={() => void toggleVisible(item)}>
            {item.visible ? "👁" : "🚫"}
          </button>
          <button class="icon" title={item.locked ? "Unlock" : "Lock"} onclick={() => void toggleLocked(item)}>
            {item.locked ? "🔒" : "🔓"}
          </button>
          <button class="label" onclick={() => selectItem(item)} ondblclick={() => openProperties(item)}>
            {item.source ?? "(unnamed)"}
          </button>
          <span class="actions">
            <button class="icon" title="Properties" onclick={() => openProperties(item)}>⚙</button>
            <button class="icon" title="Move up" disabled={idx === 0} onclick={() => void reorder(item, "up")}>▲</button>
            <button
              class="icon"
              title="Move down"
              disabled={idx === items.length - 1}
              onclick={() => void reorder(item, "down")}>▼</button
            >
            <button class="icon" title="Remove" onclick={() => void remove(item)}>🗑</button>
          </span>
        </li>
      {/each}
    </ul>
  {/if}
</div>

{#if adding}
  <AddSourceModal canvas={null} scene={currentScene} onCreated={onSourceCreated} onClose={() => (adding = false)} />
{/if}

{#if propsForSource}
  <div
    class="modal-backdrop"
    role="presentation"
    onclick={(e) => {
      if (e.target === e.currentTarget) propsForSource = null;
    }}
  >
    <div class="modal" role="dialog" aria-modal="true" aria-label="Source properties">
      <header class="modal-head">
        <h3>Properties — {propsForSource}</h3>
        <button class="icon close" title="Close" onclick={() => (propsForSource = null)}>✕</button>
      </header>
      <div class="modal-body">
        <PropertyForm kind="source" ref={propsForSource} />
      </div>
    </div>
  </div>
{/if}

<style>
  .dock {
    height: 100%;
    display: flex;
    flex-direction: column;
    background: var(--color-surface);
    font-family: var(--font-ui);
    overflow: auto;
  }
  .toolbar {
    display: flex;
    justify-content: flex-end;
    padding: 4px 6px;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .add {
    height: auto;
    padding: 2px 8px;
    font-size: 13px;
    line-height: 1;
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-muted);
  }
  .add:hover:not(:disabled) {
    color: var(--color-accent);
    border-color: var(--color-accent);
  }
  .list {
    list-style: none;
    margin: 0;
    padding: 0;
    flex: 1;
    min-height: 0;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 4px;
    padding: 4px 7px;
    border-bottom: var(--border-weight) solid var(--color-border);
    border-left: 3px solid transparent;
  }
  :global(:root[data-selection-style="left-bar"]) .row.sel {
    border-left-color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 12%, transparent);
  }
  :global(:root[data-selection-style="fill"]) .row.sel {
    background: color-mix(in srgb, var(--color-accent) 22%, transparent);
  }
  .row.sel .label {
    color: var(--color-accent);
  }
  .row.hidden-src .label {
    color: var(--color-muted);
    text-decoration: line-through;
  }
  .label {
    flex: 1;
    text-align: left;
    background: none;
    border: none;
    height: auto;
    padding: 0;
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    cursor: pointer;
  }
  .actions {
    display: none;
    gap: 2px;
  }
  .row:hover .actions {
    display: inline-flex;
  }
  .icon {
    background: none;
    border: none;
    height: auto;
    padding: 2px 4px;
    font-size: 11px;
    line-height: 1;
    color: var(--color-muted);
  }
  .icon:hover:not(:disabled) {
    color: var(--color-accent);
  }
  .icon:disabled {
    opacity: 0.3;
    cursor: default;
  }
  .msg {
    margin: 0;
    padding: 8px 7px;
    font-size: 11px;
    color: var(--color-muted);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  .msg.err {
    color: var(--color-live);
    text-transform: none;
  }
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
    width: min(560px, 100%);
    max-height: 80vh;
    display: flex;
    flex-direction: column;
  }
  .modal-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 12px 16px;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .modal-head h3 {
    margin: 0;
    font-size: 12px;
    font-family: var(--font-ui);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .modal-body {
    padding: 16px;
    overflow: auto;
  }
  .close {
    font-size: 13px;
  }
</style>
