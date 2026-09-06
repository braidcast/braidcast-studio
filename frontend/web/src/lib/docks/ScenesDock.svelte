<script lang="ts">
  import { onMount, onDestroy } from "svelte";
  import { obs, type SceneInfo, type ReorderDirection } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { selectOnMount } from "$lib/utils/focusActions";
  import { defaultCanvas } from "$lib/docks/defaultCanvasStore.svelte";
  import { canvasStore } from "$lib/stores/canvasStore.svelte";
  import { callOrToast, renamedSuffix } from "$lib/utils/callToast";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import ContextMenu, { type ContextMenuState } from "$lib/menus/ContextMenu.svelte";
  import ListToolbar, { type ToolAction } from "$lib/docking/ListToolbar.svelte";
  import FilterReveal from "$lib/docking/FilterReveal.svelte";
  import { clipboard } from "$lib/stores/clipboardStore.svelte";
  import { dockAction } from "$lib/stores/dockActionSignal.svelte";
  import { sourceSelection } from "$lib/stores/sourceSelectionStore.svelte";
  import { activeSurface } from "$lib/stores/activeSurfaceStore.svelte";
  import { openFilters } from "$lib/dialogs/filterDialogOpener.svelte";
  import {
    SceneDragReorder,
    sceneMoveActions,
    sceneOrderMenuChildren,
    type SceneOrderTarget,
  } from "$lib/utils/sceneReorder.svelte";

  // The mount adapter strips internal __* keys; this dock declares no props.
  let {}: Record<string, unknown> = $props();

  // Identifies this dock's claim on the active surface. Not the SourceSelection instance:
  // this dock drives the shared `sourceSelection` singleton it does not own, so only a
  // per-component token lets it release a claim the Sources dock may since have taken.
  const surfaceOwner = Symbol("ScenesDock");

  // Hand the surface back so a Delete after this dock closes cannot address a scene row
  // whose removal action is no longer mounted to receive it.
  onDestroy(() => {
    activeSurface.release(surfaceOwner);
  });

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

  // The toolbar acts on the current scene, the menu on the right-clicked one; both
  // index against the full, unfiltered list.
  function orderTarget(idx: number, move: (direction: ReorderDirection) => void): SceneOrderTarget {
    return { idx, count: defaultCanvas.scenes.length, disabled: filtering, move };
  }

  // Whether Remove can act at all. One predicate behind the toolbar button, the context
  // menu entry and the claim the app-level Delete reads, so the keyboard cannot offer a
  // removal the docks' own chrome disables.
  const canRemoveScene = $derived(defaultCanvas.scenes.length > 1);

  const leftActions = $derived<ToolAction[]>([
    { icon: "plus", title: "Add scene", onClick: beginAdd },
    {
      icon: "trash",
      title: "Remove scene",
      disabled: !currentName || !canRemoveScene,
      onClick: () => currentName && void remove(currentName),
    },
  ]);
  const rightActions = $derived<ToolAction[]>([
    ...sceneMoveActions(orderTarget(currentIdx, (d) => currentName && void reorder(currentName, d))),
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
  let menu = $state<ContextMenuState | null>(null);

  function report(e: unknown) {
    actionError = (e as Error).message;
  }

  // Clicking any scene row hands the app-level shortcuts to the Default surface and
  // drops the source selection with it — including when the row is already current,
  // where setCurrent no-ops and nothing downstream would clear a selection the user
  // can no longer see (Delete would then remove that stale source).
  function setCurrent(name: string) {
    actionError = null;
    activeSurface.claimScene(surfaceOwner, null, sourceSelection, name, () => canRemoveScene);
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

  // App-level F2 / Delete (scene target): run this dock's own beginRename / remove for the
  // signalled scene. The keyboard is a second caller of these actions, not a second copy:
  // the last-scene refusal and the undo entry both live in the `scenes.remove` bridge
  // method, and the error lands in this dock's `actionError` exactly as the menu's Remove
  // does. Consuming the request is what stops an unrelated scene-list mutation from
  // re-running it, and it is taken only once the scene is confirmed present, so a request
  // for a row we cannot serve is reported to its sender instead of vanishing.
  $effect(() => {
    const p = dockAction.pending;
    const action = p?.action;
    if (!p || p.canvas !== null || !action || action.kind === "renameSource") {
      return;
    }
    if (!defaultCanvas.scenes.some((s) => s.name === action.name)) {
      return;
    }
    if (!dockAction.consume(p.seq)) {
      return;
    }
    switch (action.kind) {
      case "renameScene":
        beginRename(action.name);
        break;
      case "removeScene":
        void remove(action.name);
        break;
      default:
        // A new DockAction kind must be handled here explicitly, not fall into a removal.
        action satisfies never;
    }
  });

  // A scene claim must not outlive the row it names, or the next Delete raises a
  // destructive confirm for a scene that is already gone.
  $effect(() => {
    activeSurface.dropStaleSceneClaim(
      surfaceOwner,
      defaultCanvas.scenes.map((s) => s.name),
    );
  });

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
      showToast(`Duplicated "${sceneName}"${to}${renamedSuffix(sceneName, r.name)}`, r.name);
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
  const dnd = new SceneDragReorder({
    move: (name, to) => void reorderTo(name, to),
    indexOf: (name) => defaultCanvas.scenes.findIndex((s) => s.name === name),
    disabled: () => filtering,
  });

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
    const orderChildren = sceneOrderMenuChildren(orderTarget(idx, (d) => void reorder(name, d)));
    menu = {
      x: e.clientX,
      y: e.clientY,
      items: [
        { label: "Rename", action: () => beginRename(name) },
        { label: "Filters", action: () => openFilters(name) },
        { label: "Duplicate", action: () => duplicate(name) },
        { label: "Duplicate to canvas", children: duplicateChildren },
        { label: "Order", children: orderChildren },
        // A scene is a source, so its screenshot reuses screenshot.takeSource (the
        // per-source path), targeting the scene by name instead of a scene-item id.
        {
          label: "Screenshot Scene",
          action: () => void obs.call("screenshot.takeSource", { scene: name }).catch(report),
        },
        null,
        { label: "Copy Filters", action: () => void copySceneFilters(name) },
        { label: "Paste Filters", disabled: !clipboard.filters, action: () => void pasteSceneFilters(name) },
        // Projector entries hidden pending the projector redesign.
        null,
        { label: "Remove", danger: true, disabled: !canRemoveScene, action: () => void remove(name) },
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
    <input class="inline" bind:value={renameTo} onkeydown={onRenameKey} onblur={commitRename} use:selectOnMount />
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
          class:dropTarget={dnd.isDropTarget(idx, scene.name)}
          draggable={!filtering}
          ondragstart={(e) => dnd.onDragStart(e, scene.name)}
          ondragover={(e) => dnd.onDragOver(e, idx)}
          ondrop={(e) => dnd.onDrop(e, idx)}
          ondragend={dnd.onDragEnd}
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
            use:selectOnMount
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
          class:dropTarget={dnd.isDropTarget(idx, scene.name)}
          draggable={!filtering}
          ondragstart={(e) => dnd.onDragStart(e, scene.name)}
          ondragover={(e) => dnd.onDragOver(e, idx)}
          ondrop={(e) => dnd.onDrop(e, idx)}
          ondragend={dnd.onDragEnd}
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
            use:selectOnMount
          />
        </li>
      {/if}
    </ul>

    {#if filteredScenes.length === 0 && !adding}
      <p class="dock-msg">{filter.trim() ? "No matches" : "No scenes"}</p>
    {/if}
  {/if}

  {#if actionError}
    <p class="dock-msg err" role="alert">{actionError}</p>
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
  /* Grid twin of the row focus ring in app.css — see the note there for why the indicator
     sits on the tile, why it is --color-text rather than the accent, and why :where(). */
  .grid-tile:where(:has(.dock-label:focus-visible)) {
    outline: 2px solid var(--color-text);
    outline-offset: -2px;
  }
  .grid-tile .dock-label:focus-visible {
    outline: none;
  }
</style>
