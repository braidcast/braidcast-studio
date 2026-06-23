<script lang="ts">
  import { onMount } from "svelte";
  import { obs, type SceneItem, type ReorderDirection } from "../bridge";
  import { defaultCanvas } from "./defaultCanvasStore.svelte";
  import AddSourceModal from "../AddSourceModal.svelte";
  import PropertyForm from "../properties/PropertyForm.svelte";
  import { suspendPreview } from "../previewGate.svelte";
  import ContextMenu, { type ContextMenuItem } from "../ContextMenu.svelte";

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
  let renamingId = $state<number | null>(null);
  let renameTo = $state("");
  let menu = $state<{ x: number; y: number; items: (ContextMenuItem | null)[] } | null>(null);

  function report(e: unknown) {
    error = (e as Error).message;
  }

  function focusOnMount(node: HTMLInputElement) {
    node.focus();
    node.select();
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
      // Global channel-0 path: only the Default surface (canvas=null) drives this.
      if (p.canvas == null && (!p.scene || p.scene === currentScene)) {
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

  function beginRename(item: SceneItem) {
    renamingId = item.id;
    renameTo = item.source ?? "";
  }

  async function commitRename() {
    const id = renamingId;
    const name = renameTo.trim();
    renamingId = null;
    if (id === null || !name) {
      return;
    }
    try {
      await obs.call("sources.rename", { scene: currentScene, id, name });
    } catch (e) {
      report(e);
    }
  }

  function onRenameKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      void commitRename();
    } else if (e.key === "Escape") {
      renamingId = null;
    }
  }

  function openMenu(e: MouseEvent, item: SceneItem, idx: number) {
    e.preventDefault();
    menu = {
      x: e.clientX,
      y: e.clientY,
      items: [
        { label: "Properties", action: () => openProperties(item) },
        { label: "Rename", action: () => beginRename(item) },
        null,
        { label: item.visible ? "Hide" : "Show", action: () => void toggleVisible(item) },
        { label: item.locked ? "Unlock" : "Lock", action: () => void toggleLocked(item) },
        null,
        { label: "Move Up", disabled: idx === 0, action: () => void reorder(item, "up") },
        { label: "Move Down", disabled: idx === items.length - 1, action: () => void reorder(item, "down") },
        { label: "Move to Top", disabled: idx === 0, action: () => void reorder(item, "top") },
        { label: "Move to Bottom", disabled: idx === items.length - 1, action: () => void reorder(item, "bottom") },
        null,
        { label: "Remove", danger: true, action: () => void remove(item) },
      ],
    };
  }
</script>

<div class="dock-body">
  <div class="dock-toolbar">
    <button class="dock-add" title="Add source" disabled={!currentScene} onclick={() => (adding = true)}>＋</button>
  </div>

  {#if error}
    <p class="dock-msg err">{error}</p>
  {:else if !currentScene}
    <p class="dock-msg">No scene selected</p>
  {:else if !loaded}
    <p class="dock-msg">Loading…</p>
  {:else if items.length === 0}
    <p class="dock-msg">No sources</p>
  {:else}
    <ul class="dock-list">
      {#each items as item, idx (item.id)}
        <li
          class="dock-row"
          class:sel={item.id === selectedItemId}
          class:dimmed={!item.visible}
          oncontextmenu={(e) => openMenu(e, item, idx)}
        >
          <button class="dock-icon" title={item.visible ? "Hide" : "Show"} onclick={() => void toggleVisible(item)}>
            {item.visible ? "👁" : "🚫"}
          </button>
          <button class="dock-icon" title={item.locked ? "Unlock" : "Lock"} onclick={() => void toggleLocked(item)}>
            {item.locked ? "🔒" : "🔓"}
          </button>
          {#if renamingId === item.id}
            <input class="inline" bind:value={renameTo} onkeydown={onRenameKey} onblur={commitRename} use:focusOnMount />
          {:else}
            <button class="dock-label" onclick={() => selectItem(item)} ondblclick={() => openProperties(item)}>
              {item.source ?? "(unnamed)"}
            </button>
          {/if}
          <span class="dock-actions">
            <button class="dock-icon" title="Properties" onclick={() => openProperties(item)}>⚙</button>
            <button class="dock-icon" title="Move up" disabled={idx === 0} onclick={() => void reorder(item, "up")}>▲</button>
            <button
              class="dock-icon"
              title="Move down"
              disabled={idx === items.length - 1}
              onclick={() => void reorder(item, "down")}>▼</button
            >
            <button class="dock-icon" title="Remove" onclick={() => void remove(item)}>🗑</button>
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
        <button class="dock-icon close" title="Close" onclick={() => (propsForSource = null)}>✕</button>
      </header>
      <div class="modal-body">
        <PropertyForm kind="source" ref={propsForSource} />
      </div>
    </div>
  </div>
{/if}

{#if menu}
  <ContextMenu x={menu.x} y={menu.y} items={menu.items} onClose={() => (menu = null)} />
{/if}

<style>
  /* Sources rows are one row shorter than the shared default (5px 7px). */
  .dock-row {
    padding: 4px 7px;
  }
  .inline {
    flex: 1;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-accent);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 3px 5px;
  }
  .inline:focus {
    outline: none;
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
