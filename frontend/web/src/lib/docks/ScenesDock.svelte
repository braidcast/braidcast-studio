<script lang="ts">
  import { onMount } from "svelte";
  import { obs, type SceneInfo, type ReorderDirection } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { defaultCanvas } from "$lib/docks/defaultCanvasStore.svelte";
  import { canvasStore } from "$lib/stores/canvasStore.svelte";
  import { callOrToast } from "$lib/utils/callToast";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import ContextMenu, { type ContextMenuItem } from "$lib/menus/ContextMenu.svelte";
  import ListToolbar, { type ToolAction } from "$lib/docking/ListToolbar.svelte";
  import FilterReveal from "$lib/docking/FilterReveal.svelte";
  import { clipboard } from "$lib/stores/clipboardStore.svelte";
  import { openFilters } from "$lib/dialogs/filterDialogOpener.svelte";

  // The mount adapter strips internal __* keys; this dock declares no props.
  let {}: Record<string, unknown> = $props();

  onMount(() => {
    defaultCanvas.start();
    canvasStore.start();
    obs
      .call("settings.getGeneral")
      .then((g) => (gridMode = g.scenesGridMode))
      .catch(() => {});
    const offGeneral = obs.on(EV.settingsGeneralChanged, (g) => (gridMode = g.scenesGridMode));
    return () => offGeneral();
  });

  // Session-only name filter; grid/list layout mirrors the persisted general setting.
  // Reorder (buttons, menu, drag) is disabled while filtering since indices only
  // make sense against the full, unfiltered ordering.
  let filter = $state("");
  let gridMode = $state(false);
  const filtering = $derived(filter.trim().length > 0);

  const filteredScenes = $derived(
    filtering
      ? defaultCanvas.scenes.filter((s) => s.name.toLowerCase().includes(filter.trim().toLowerCase()))
      : defaultCanvas.scenes,
  );

  function toggleGrid() {
    const next = !gridMode;
    gridMode = next; // optimistic; generalChanged reconciles
    obs.call("settings.setGeneral", { scenesGridMode: next }).catch(report);
  }

  // The bottom toolbar acts on the current (selected) scene, mirroring the
  // per-canvas CanvasDock chrome. Rename stays on dbl-click + the context menu.
  const currentName = $derived(defaultCanvas.scenes.find((s) => s.current)?.name ?? null);
  // Reorder is indexed against the full, unfiltered scene list so up/down
  // disable at the ends (mirrors SourcesDock's selectedIdx).
  const currentIdx = $derived(
    currentName === null ? -1 : defaultCanvas.scenes.findIndex((s) => s.name === currentName),
  );

  const leftActions = $derived<ToolAction[]>([
    { icon: "plus", title: "Add scene", onClick: beginAdd },
    {
      icon: "trash",
      title: "Remove scene",
      disabled: !currentName || defaultCanvas.scenes.length <= 1,
      onClick: () => currentName && void remove(currentName),
    },
  ]);
  const rightActions = $derived<ToolAction[]>([
    {
      icon: "up",
      title: "Move up",
      disabled: filtering || currentIdx <= 0,
      onClick: () => currentName && void reorder(currentName, "up"),
    },
    {
      icon: "down",
      title: "Move down",
      disabled: filtering || currentIdx < 0 || currentIdx >= defaultCanvas.scenes.length - 1,
      onClick: () => currentName && void reorder(currentName, "down"),
    },
    {
      icon: gridMode ? "list" : "grid",
      title: gridMode ? "List view" : "Grid view",
      active: gridMode,
      onClick: toggleGrid,
    },
  ]);

  let adding = $state(false);
  let newName = $state("");
  let renamingFrom = $state<string | null>(null);
  let renameTo = $state("");
  let actionError = $state<string | null>(null);
  let menu = $state<{ x: number; y: number; items: (ContextMenuItem | null)[] } | null>(null);

  function focusOnMount(node: HTMLInputElement) {
    node.focus();
    node.select();
  }

  function report(e: unknown) {
    actionError = (e as Error).message;
  }

  function setCurrent(name: string) {
    actionError = null;
    defaultCanvas.setCurrent(name).catch(report);
  }

  function beginAdd() {
    actionError = null;
    adding = true;
    newName = "";
  }

  async function commitAdd() {
    const name = newName.trim();
    adding = false;
    if (!name) {
      return;
    }
    actionError = null;
    try {
      await defaultCanvas.create(name);
    } catch (e) {
      report(e);
    }
  }

  function beginRename(name: string) {
    actionError = null;
    renamingFrom = name;
    renameTo = name;
  }

  async function commitRename() {
    const from = renamingFrom;
    const to = renameTo.trim();
    renamingFrom = null;
    if (!from || !to || to === from) {
      return;
    }
    actionError = null;
    try {
      await defaultCanvas.rename(from, to);
    } catch (e) {
      report(e);
    }
  }

  function duplicate(name: string) {
    actionError = null;
    defaultCanvas.duplicate(name).catch(report);
  }

  async function copySceneFilters(name: string) {
    actionError = null;
    try {
      clipboard.filters = (await obs.call("filters.copyChain", { source: name })).filters;
    } catch (e) {
      report(e);
    }
  }

  async function pasteSceneFilters(name: string) {
    if (!clipboard.filters) {
      return;
    }
    actionError = null;
    try {
      await obs.call("filters.pasteChain", { source: name, filters: clipboard.filters });
    } catch (e) {
      report(e);
    }
  }

  // Duplicates a scene from the Default canvas onto another canvas (a deep copy,
  // unlike the same-canvas duplicate() above which is a ref duplicate). No `canvas`
  // param is sent, mirroring defaultCanvasStore's other calls (omitted = Default).
  // See scenes.duplicateToCanvas in bridge.ts.
  async function duplicateToCanvas(sceneName: string, destUuid: string) {
    const r = await callOrToast("scenes.duplicateToCanvas", { name: sceneName, destCanvas: destUuid }, "Duplicate failed");
    if (r) {
      const destName = canvasStore.byUuid(destUuid)?.name;
      const to = destName ? ` to "${destName}"` : "";
      const as = r.name !== sceneName ? ` as "${r.name}"` : "";
      showToast(`Duplicated "${sceneName}"${to}${as}`, r.name);
    }
  }

  async function remove(name: string) {
    actionError = null;
    try {
      await defaultCanvas.remove(name);
    } catch (e) {
      report(e);
    }
  }

  async function reorder(name: string, direction: ReorderDirection) {
    actionError = null;
    try {
      await obs.call("scenes.reorder", { name, direction });
    } catch (e) {
      report(e);
    }
  }

  // Drag-to-reorder. `to` is the drop row's top-first index (the same order this
  // list renders and scenes.list returns); the bridge moves the dragged scene
  // there, same as the up/down buttons. Disabled while filtering, since indices
  // only make sense against the full ordering.
  let dragName = $state<string | null>(null);
  let dragOverIdx = $state<number | null>(null);

  function onDragStart(e: DragEvent, scene: SceneInfo) {
    if (filtering) {
      return;
    }
    dragName = scene.name;
    if (e.dataTransfer) {
      e.dataTransfer.effectAllowed = "move";
      e.dataTransfer.setData("text/plain", scene.name); // Firefox requires data
    }
  }

  function onDragOver(e: DragEvent, idx: number) {
    if (dragName === null) {
      return;
    }
    e.preventDefault(); // mark this a valid drop target
    if (e.dataTransfer) {
      e.dataTransfer.dropEffect = "move";
    }
    dragOverIdx = idx;
  }

  function onDrop(e: DragEvent, idx: number) {
    e.preventDefault();
    const name = dragName;
    dragName = null;
    dragOverIdx = null;
    if (name === null) {
      return;
    }
    const from = defaultCanvas.scenes.findIndex((s) => s.name === name);
    if (from < 0 || from === idx) {
      return;
    }
    void reorderTo(name, idx);
  }

  function onDragEnd() {
    dragName = null;
    dragOverIdx = null;
  }

  async function reorderTo(name: string, to: number) {
    actionError = null;
    try {
      await obs.call("scenes.reorder", { name, to });
    } catch (e) {
      report(e);
    }
  }

  function openMenu(e: MouseEvent, name: string) {
    e.preventDefault();
    const otherCanvases = canvasStore.canvases.filter((c) => !c.isDefault);
    const duplicateChildren =
      otherCanvases.length === 0
        ? [{ label: "(no other canvases)", disabled: true }]
        : otherCanvases.map((c) => ({
            label: c.name,
            action: () => void duplicateToCanvas(name, c.uuid),
          }));
    const idx = defaultCanvas.scenes.findIndex((s) => s.name === name);
    const orderChildren = [
      { label: "Up", disabled: filtering || idx <= 0, action: () => void reorder(name, "up") },
      {
        label: "Down",
        disabled: filtering || idx < 0 || idx >= defaultCanvas.scenes.length - 1,
        action: () => void reorder(name, "down"),
      },
      { label: "Top", disabled: filtering || idx <= 0, action: () => void reorder(name, "top") },
      {
        label: "Bottom",
        disabled: filtering || idx < 0 || idx >= defaultCanvas.scenes.length - 1,
        action: () => void reorder(name, "bottom"),
      },
    ];
    menu = {
      x: e.clientX,
      y: e.clientY,
      items: [
        { label: "Rename", action: () => beginRename(name) },
        { label: "Filters", action: () => openFilters(name) },
        { label: "Duplicate", action: () => duplicate(name) },
        { label: "Duplicate to canvas", children: duplicateChildren },
        { label: "Order", children: orderChildren },
        null,
        { label: "Copy Filters", action: () => void copySceneFilters(name) },
        { label: "Paste Filters", disabled: !clipboard.filters, action: () => void pasteSceneFilters(name) },
        // Projector entries hidden pending the projector redesign.
        null,
        { label: "Remove", danger: true, disabled: defaultCanvas.scenes.length <= 1, action: () => void remove(name) },
      ],
    };
  }

  function onAddKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      void commitAdd();
    } else if (e.key === "Escape") {
      adding = false;
    }
  }

  function onRenameKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      void commitRename();
    } else if (e.key === "Escape") {
      renamingFrom = null;
    }
  }
</script>

<!-- Per-scene label/rename, shared by both layouts so selection, activation, and
     rename behave identically in list and grid. -->
{#snippet sceneCell(scene: SceneInfo)}
  {#if renamingFrom === scene.name}
    <input class="inline" bind:value={renameTo} onkeydown={onRenameKey} onblur={commitRename} use:focusOnMount />
  {:else}
    <button class="dock-label" ondblclick={() => beginRename(scene.name)} onclick={() => setCurrent(scene.name)}>
      {scene.name}
    </button>
  {/if}
{/snippet}

<div class="dock-body">
  <div class="dock-fill">
    {#if defaultCanvas.error}
      <p class="dock-msg err">{defaultCanvas.error}</p>
    {:else if !defaultCanvas.loaded}
      <p class="dock-msg">Loading…</p>
    {:else if gridMode}
    <div class="scene-grid">
      {#each filteredScenes as scene, idx (scene.name)}
        <div
          class="grid-tile"
          class:sel={scene.current}
          class:dropTarget={dragOverIdx === idx && dragName !== null && dragName !== scene.name}
          draggable={!filtering}
          ondragstart={(e) => onDragStart(e, scene)}
          ondragover={(e) => onDragOver(e, idx)}
          ondrop={(e) => onDrop(e, idx)}
          ondragend={onDragEnd}
          oncontextmenu={(e) => openMenu(e, scene.name)}
          role="listitem"
        >
          {@render sceneCell(scene)}
        </div>
      {/each}

      {#if adding}
        <div class="grid-tile">
          <input
            class="inline"
            placeholder="Scene name"
            bind:value={newName}
            onkeydown={onAddKey}
            onblur={commitAdd}
            use:focusOnMount
          />
        </div>
      {/if}
    </div>

    {#if filteredScenes.length === 0 && !adding}
      <p class="dock-msg">{filter.trim() ? "No matches" : "No scenes"}</p>
    {/if}
  {:else}
    <ul class="dock-list">
      {#each filteredScenes as scene, idx (scene.name)}
        <li
          class="dock-row"
          class:sel={scene.current}
          class:dropTarget={dragOverIdx === idx && dragName !== null && dragName !== scene.name}
          draggable={!filtering}
          ondragstart={(e) => onDragStart(e, scene)}
          ondragover={(e) => onDragOver(e, idx)}
          ondrop={(e) => onDrop(e, idx)}
          ondragend={onDragEnd}
          oncontextmenu={(e) => openMenu(e, scene.name)}
        >
          {@render sceneCell(scene)}
        </li>
      {/each}

      {#if adding}
        <li class="dock-row">
          <input
            class="inline"
            placeholder="Scene name"
            bind:value={newName}
            onkeydown={onAddKey}
            onblur={commitAdd}
            use:focusOnMount
          />
        </li>
      {/if}
    </ul>

    {#if filteredScenes.length === 0 && !adding}
      <p class="dock-msg">{filter.trim() ? "No matches" : "No scenes"}</p>
    {/if}
  {/if}

  {#if actionError}
    <p class="dock-msg err">{actionError}</p>
  {/if}
  </div>

  <ListToolbar left={leftActions} right={rightActions}>
    {#snippet middle()}
      <FilterReveal bind:value={filter} />
    {/snippet}
  </ListToolbar>
</div>

{#if menu}
  <ContextMenu x={menu.x} y={menu.y} items={menu.items} onClose={() => (menu = null)} />
{/if}

<style>
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
  /* Drag-reorder drop indicator. Outline avoids layout shift. */
  .dock-row.dropTarget,
  .grid-tile.dropTarget {
    outline: var(--border-weight) solid var(--color-accent);
    outline-offset: -1px;
  }
  /* Scroll region above the pinned bottom toolbar. */
  .dock-fill {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
    overflow: auto;
  }
  .scene-grid {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(96px, 1fr));
    gap: 4px;
    padding: 6px;
    align-content: start;
  }
  .grid-tile {
    display: flex;
    align-items: center;
    min-height: 44px;
    padding: 6px 8px;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    border-left: 3px solid transparent;
  }
  :root[data-selection-style="left-bar"] .grid-tile.sel {
    border-left-color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 12%, transparent);
  }
  :root[data-selection-style="fill"] .grid-tile.sel {
    background: color-mix(in srgb, var(--color-accent) 22%, transparent);
  }
  .grid-tile.sel :global(.dock-label) {
    color: var(--color-accent);
  }
</style>
