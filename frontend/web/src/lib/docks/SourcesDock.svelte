<script lang="ts">
  import { onMount, onDestroy } from "svelte";
  import { obs, type SceneItem, type ReorderDirection } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { defaultCanvas } from "$lib/docks/defaultCanvasStore.svelte";
  import AddSourceModal from "$lib/dialogs/add-source/AddSourceModal.svelte";
  import PropertiesModal from "$lib/properties/PropertiesModal.svelte";
  import { suspendPreview } from "$lib/stores/previewGate.svelte";
  import ContextMenu, { type ContextMenuItem } from "$lib/menus/ContextMenu.svelte";
  import { clipboard } from "$lib/stores/clipboardStore.svelte";
  import { copyItem, pasteReference, pasteDuplicate } from "$lib/stores/clipboardItemState";
  import { sourceSelection } from "$lib/stores/sourceSelectionStore.svelte";
  import { openFilters } from "$lib/dialogs/filterDialogOpener.svelte";
  import { transformMenu } from "$lib/menus/transformMenu";
  import { scaleFilterMenu } from "$lib/menus/scaleFilterMenu";
  import { blendModeMenu, blendMethodMenu } from "$lib/menus/blendMenu";
  import { showTransitionMenu, hideTransitionMenu, transitionTypes } from "$lib/menus/transitionMenu";
  import { deinterlaceMenu } from "$lib/menus/deinterlaceMenu";
  import { colorMenu } from "$lib/menus/colorMenu";
  import type { DeinterlaceMode, DeinterlaceFieldOrder } from "$lib/api/bridge";
  import Icon from "$lib/ui/Icon.svelte";
  import ListToolbar, { type ToolAction } from "$lib/docking/ListToolbar.svelte";
  import FilterReveal from "$lib/docking/FilterReveal.svelte";

  let {}: Record<string, unknown> = $props();

  onMount(() => {
    defaultCanvas.start();
  });

  // The default canvas's current scene drives this list (global channel-0 path).
  const currentScene = $derived(defaultCanvas.current);

  let items = $state<SceneItem[]>([]);
  // Session-only name filter. Reorder is disabled while filtering since the
  // up/down indices only make sense against the full, unfiltered ordering.
  let filter = $state("");
  const filtering = $derived(filter.trim().length > 0);
  const filteredItems = $derived(
    filtering ? items.filter((i) => (i.source ?? "").toLowerCase().includes(filter.trim().toLowerCase())) : items,
  );
  let loaded = $state(false);
  let error = $state<string | null>(null);
  let propsForSource = $state<string | null>(null);
  let adding = $state(false);
  let renamingId = $state<number | null>(null);
  let renameTo = $state("");
  let menu = $state<{ x: number; y: number; items: (ContextMenuItem | null)[] } | null>(null);

  // The bottom toolbar acts on the selected row (OBS list convention). Reorder is
  // indexed against the full, unfiltered `items` so up/down disable at the ends.
  const selectedItem = $derived(sourceSelection.item);
  const selectedIdx = $derived(selectedItem ? items.findIndex((i) => i.id === selectedItem.id) : -1);

  const leftActions = $derived<ToolAction[]>([
    { icon: "plus", title: "Add source", disabled: !currentScene, onClick: () => (adding = true) },
    { icon: "trash", title: "Remove source", disabled: !selectedItem, onClick: () => void removeSelected() },
    {
      icon: "gear",
      title: "Properties",
      disabled: !selectedItem?.source,
      onClick: () => selectedItem && openProperties(selectedItem),
    },
  ]);
  const rightActions = $derived<ToolAction[]>([
    {
      icon: "up",
      title: "Move up",
      disabled: filtering || selectedIdx <= 0,
      onClick: () => selectedItem && void reorder(selectedItem, "up"),
    },
    {
      icon: "down",
      title: "Move down",
      disabled: filtering || selectedIdx < 0 || selectedIdx >= items.length - 1,
      onClick: () => selectedItem && void reorder(selectedItem, "down"),
    },
  ]);

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

  // Plain click selects just this row; Ctrl/Cmd toggles it in/out of the set; Shift
  // extends from the anchor over the visible (filtered) order. Only a plain click drives
  // the native preview selection — a modifier click is a list-set edit, not a preview pick.
  function selectItem(e: MouseEvent, item: SceneItem) {
    if (e.shiftKey) {
      sourceSelection.range(item, filteredItems);
    } else if (e.ctrlKey || e.metaKey) {
      sourceSelection.toggle(item);
    } else {
      sourceSelection.selectOne(item);
      void obs.call("preview.select", { scene: currentScene, id: item.id }).catch(() => {});
    }
  }

  // The properties modal overlaps the preview; suspend the native overlay while open.
  $effect(() => {
    if (propsForSource) {
      return suspendPreview();
    }
  });

  // Reflect preview-driven selection (click in the overlay) back into the list.
  $effect(() => {
    return obs.on(EV.sceneItemSelected, (p) => {
      // Global channel-0 path: only the Default surface (canvas=null) drives this.
      if (p.canvas == null && (!p.scene || p.scene === currentScene)) {
        const it = items.find((i) => i.id === p.id);
        if (it) {
          sourceSelection.selectOne(it);
        }
      }
    });
  });

  // Request-generation guard: a fast scene switch must not let a slow prior list
  // response overwrite the newer scene's sources.
  let loadSeq = 0;
  async function load() {
    if (!currentScene) {
      items = [];
      loaded = true;
      return;
    }
    const mine = ++loadSeq;
    error = null;
    try {
      const list = await obs.call("sceneItems.list", { scene: currentScene });
      if (mine !== loadSeq) {
        return;
      }
      items = list;
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

  // Publish/refresh the global selection so the app-level Delete / Ctrl+C / Ctrl+V act
  // on it. reconcile refreshes the held items against the latest list and clears the set
  // on a scene change (per-scene selection).
  $effect(() => {
    sourceSelection.reconcile(currentScene, items);
  });

  // Drop our published selection on teardown so the app-level shortcuts don't act on a
  // stale scene/item after this dock unmounts — but only if the store still points at
  // what we published (another surface may have taken over).
  onDestroy(() => {
    if (sourceSelection.scene === currentScene) {
      sourceSelection.scene = null;
      sourceSelection.clear();
    }
  });

  // Refresh on item mutations targeting the global path (canvas=null) for our scene.
  $effect(() => {
    return obs.on(EV.sceneItemsChanged, (p) => {
      if (p.canvas == null && (!p.scene || p.scene === currentScene)) {
        void load();
      }
    });
  });

  function onSourceCreated(created: { id: number; source: string }) {
    adding = false;
    propsForSource = created.source;
    void load().then(() => {
      const it = items.find((i) => i.id === created.id);
      if (it) {
        sourceSelection.selectOne(it);
      }
    });
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

  // Drag-to-reorder. `to` is the drop row's top-first index (the same order this
  // list renders and sceneItems.list returns); the bridge moves the dragged item
  // there as one undo action, the same as the up/down buttons. Disabled while
  // filtering, since indices only make sense against the full ordering.
  let dragId = $state<number | null>(null);
  let dragOverIdx = $state<number | null>(null);

  function onDragStart(e: DragEvent, item: SceneItem) {
    if (filtering) {
      return;
    }
    dragId = item.id;
    if (e.dataTransfer) {
      e.dataTransfer.effectAllowed = "move";
      e.dataTransfer.setData("text/plain", String(item.id)); // Firefox requires data
    }
  }

  function onDragOver(e: DragEvent, idx: number) {
    if (dragId === null) {
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
    const id = dragId;
    dragId = null;
    dragOverIdx = null;
    if (id === null) {
      return;
    }
    const from = items.findIndex((i) => i.id === id);
    if (from < 0 || from === idx) {
      return;
    }
    void reorderTo(id, idx);
  }

  function onDragEnd() {
    dragId = null;
    dragOverIdx = null;
  }

  async function reorderTo(id: number, to: number) {
    try {
      await obs.call("sceneItems.reorder", { scene: currentScene, id, to });
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

  // Batch remove: loop the existing single-item bridge remove once per selected id (no
  // new bridge method; each removal stays independently undoable). Falls back to the
  // primary row when nothing is in the set. Snapshot the ids first — the removals fire
  // reload events that shrink the set mid-loop.
  async function removeSelected() {
    const ids = sourceSelection.size > 0 ? [...sourceSelection.ids] : selectedItem ? [selectedItem.id] : [];
    for (const id of ids) {
      try {
        await obs.call("sceneItems.remove", { scene: currentScene, id });
      } catch (e) {
        report(e);
      }
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

  // ---- clipboard actions (copy/paste/duplicate/filters/transform/group) ------
  // All target the global channel-0 path (no canvas); paste lands in currentScene.
  function copySource(item: SceneItem) {
    void copyItem({ scene: currentScene, id: item.id }, item);
  }

  async function pasteSource() {
    try {
      await pasteReference({ scene: currentScene });
    } catch (e) {
      report(e);
    }
  }

  async function pasteDuplicateSource() {
    try {
      await pasteDuplicate({ scene: currentScene });
    } catch (e) {
      report(e);
    }
  }

  async function duplicateItem(item: SceneItem) {
    try {
      await obs.call("sources.duplicate", { scene: currentScene, id: item.id });
    } catch (e) {
      report(e);
    }
  }

  async function copyFilters(item: SceneItem) {
    if (!item.source) {
      return;
    }
    try {
      clipboard.filters = (await obs.call("filters.copyChain", { source: item.source })).filters;
    } catch (e) {
      report(e);
    }
  }

  async function pasteFilters(item: SceneItem) {
    if (!item.source || !clipboard.filters) {
      return;
    }
    try {
      await obs.call("filters.pasteChain", { source: item.source, filters: clipboard.filters });
    } catch (e) {
      report(e);
    }
  }

  async function copyTransform(item: SceneItem) {
    try {
      clipboard.transform = await obs.call("sceneItems.getTransform", { scene: currentScene, id: item.id });
    } catch (e) {
      report(e);
    }
  }

  async function pasteTransform(item: SceneItem) {
    if (!clipboard.transform) {
      return;
    }
    try {
      await obs.call("sceneItems.setTransform", { scene: currentScene, id: item.id, transform: clipboard.transform });
    } catch (e) {
      report(e);
    }
  }

  async function groupItem(item: SceneItem) {
    try {
      await obs.call("sceneItems.group", { scene: currentScene, ids: [item.id] });
    } catch (e) {
      report(e);
    }
  }

  async function ungroupItem(item: SceneItem) {
    try {
      await obs.call("sceneItems.ungroup", { scene: currentScene, id: item.id });
    } catch (e) {
      report(e);
    }
  }

  // Deinterlacing lives on the source (not the scene item), so it isn't in the row
  // data; fetch it just-in-time when the menu opens. Falls back to disabled state.
  async function fetchDeint(source: string): Promise<{ mode: DeinterlaceMode; fieldOrder: DeinterlaceFieldOrder }> {
    try {
      return await obs.call("sources.getDeinterlace", { source });
    } catch {
      return { mode: "disable", fieldOrder: "top" };
    }
  }

  async function openMenu(e: MouseEvent, item: SceneItem, idx: number) {
    e.preventDefault();
    const x = e.clientX;
    const y = e.clientY;
    const deint = item.source ? await fetchDeint(item.source) : { mode: "disable" as const, fieldOrder: "top" as const };
    const transitionTypeList = await transitionTypes().catch(() => []);
    // Right-clicking a row that is part of a 2+ selection acts on the whole set; a
    // right-click on a single (or unselected) row stays single.
    const inMulti = sourceSelection.has(item.id) && sourceSelection.size >= 2;
    menu = {
      x,
      y,
      items: [
        { label: "Properties", action: () => openProperties(item) },
        { label: "Filters", disabled: !item.source, action: () => item.source && openFilters(item.source) },
        ...(item.interactive && item.source
          ? [{ label: "Interact", action: () => void obs.call("sources.interact", { source: item.source }).catch(report) }]
          : []),
        transformMenu({ scene: currentScene ?? undefined, id: item.id }, item.source ?? "(unnamed)"),
        { label: "Rename", action: () => beginRename(item) },
        scaleFilterMenu(item.scaleFilter, (filter) =>
          void obs.call("sceneItems.setScaleFilter", { scene: currentScene, id: item.id, filter }).catch(report),
        ),
        blendModeMenu(item.blendMode, (mode) =>
          void obs.call("sceneItems.setBlendingMode", { scene: currentScene, id: item.id, mode }).catch(report),
        ),
        blendMethodMenu(item.blendMethod, (method) =>
          void obs.call("sceneItems.setBlendingMethod", { scene: currentScene, id: item.id, method }).catch(report),
        ),
        showTransitionMenu(
          item.showTransition,
          transitionTypeList,
          (type) =>
            void obs
              .call("sceneItems.setShowTransition", {
                scene: currentScene,
                id: item.id,
                transition: type,
                duration: item.showTransition?.duration ?? 300,
              })
              .catch(report),
          (duration) =>
            void obs
              .call("sceneItems.setShowTransition", {
                scene: currentScene,
                id: item.id,
                transition: item.showTransition?.type ?? null,
                duration,
              })
              .catch(report),
        ),
        hideTransitionMenu(
          item.hideTransition,
          transitionTypeList,
          (type) =>
            void obs
              .call("sceneItems.setHideTransition", {
                scene: currentScene,
                id: item.id,
                transition: type,
                duration: item.hideTransition?.duration ?? 300,
              })
              .catch(report),
          (duration) =>
            void obs
              .call("sceneItems.setHideTransition", {
                scene: currentScene,
                id: item.id,
                transition: item.hideTransition?.type ?? null,
                duration,
              })
              .catch(report),
        ),
        ...(item.source
          ? [
              deinterlaceMenu(
                deint.mode,
                deint.fieldOrder,
                (mode) => void obs.call("sources.setDeinterlace", { source: item.source, mode }).catch(report),
                (fieldOrder) =>
                  void obs.call("sources.setDeinterlace", { source: item.source, fieldOrder }).catch(report),
              ),
            ]
          : []),
        colorMenu(item.color, (color) =>
          void obs.call("sceneItems.setColor", { scene: currentScene, id: item.id, color }).catch(report),
        ),
        {
          label: "Screenshot",
          disabled: !item.source,
          action: () => void obs.call("screenshot.takeSource", { scene: currentScene, id: item.id }).catch(report),
        },
        null,
        { label: "Copy", disabled: !item.source, action: () => copySource(item) },
        { label: "Paste", disabled: !clipboard.source, action: () => void pasteSource() },
        { label: "Paste (Duplicate)", disabled: !clipboard.source, action: () => void pasteDuplicateSource() },
        { label: "Duplicate", action: () => void duplicateItem(item) },
        null,
        { label: "Copy Filters", disabled: !item.source, action: () => void copyFilters(item) },
        { label: "Paste Filters", disabled: !clipboard.filters, action: () => void pasteFilters(item) },
        { label: "Copy Transform", action: () => void copyTransform(item) },
        { label: "Paste Transform", disabled: !clipboard.transform, action: () => void pasteTransform(item) },
        null,
        { label: "Group", action: () => void groupItem(item) },
        { label: "Ungroup", action: () => void ungroupItem(item) },
        null,
        { label: item.visible ? "Hide" : "Show", action: () => void toggleVisible(item) },
        { label: item.locked ? "Unlock" : "Lock", action: () => void toggleLocked(item) },
        null,
        { label: "Move Up", disabled: filtering || idx === 0, action: () => void reorder(item, "up") },
        { label: "Move Down", disabled: filtering || idx === items.length - 1, action: () => void reorder(item, "down") },
        { label: "Move to Top", disabled: filtering || idx === 0, action: () => void reorder(item, "top") },
        {
          label: "Move to Bottom",
          disabled: filtering || idx === items.length - 1,
          action: () => void reorder(item, "bottom"),
        },
        // Projector entries hidden pending the projector redesign (projectorMenu +
        // its bridge path are kept, just not surfaced here).
        null,
        {
          label: inMulti ? `Remove ${sourceSelection.size} Items` : "Remove",
          danger: true,
          action: () => void (inMulti ? removeSelected() : remove(item)),
        },
      ],
    };
  }
</script>

<div class="dock-body">
  <div class="dock-fill">
    {#if error}
      <p class="dock-msg err">{error}</p>
    {:else if !currentScene}
      <p class="dock-msg">No scene selected</p>
    {:else if !loaded}
      <p class="dock-msg">Loading…</p>
    {:else if items.length === 0}
      <p class="dock-msg">No sources</p>
    {:else if filteredItems.length === 0}
      <p class="dock-msg">No matches</p>
    {:else}
      <ul class="dock-list">
      {#each filteredItems as item, idx (item.id)}
        <li
          class="dock-row"
          class:sel={sourceSelection.has(item.id)}
          class:dimmed={!item.visible}
          class:dropTarget={dragOverIdx === idx && dragId !== null && dragId !== item.id}
          style:box-shadow={item.color ? `inset 3px 0 0 ${item.color}` : null}
          draggable={!filtering}
          ondragstart={(e) => onDragStart(e, item)}
          ondragover={(e) => onDragOver(e, idx)}
          ondrop={(e) => onDrop(e, idx)}
          ondragend={onDragEnd}
          oncontextmenu={(e) => void openMenu(e, item, idx)}
        >
          <button
            class="dock-toggle"
            class:off={!item.visible}
            title={item.visible ? "Hide" : "Show"}
            aria-label={item.visible ? "Hide" : "Show"}
            onclick={() => void toggleVisible(item)}><Icon name={item.visible ? "eye" : "eye-off"} size={14} /></button
          >
          <button
            class="dock-toggle"
            title={item.locked ? "Unlock" : "Lock"}
            aria-label={item.locked ? "Unlock" : "Lock"}
            onclick={() => void toggleLocked(item)}><Icon name={item.locked ? "lock" : "lock-open"} size={12} /></button
          >
          {#if renamingId === item.id}
            <input class="inline" bind:value={renameTo} onkeydown={onRenameKey} onblur={commitRename} use:focusOnMount />
          {:else}
            <button class="dock-label" onclick={(e) => selectItem(e, item)} ondblclick={() => openProperties(item)}>
              {item.source ?? "(unnamed)"}
            </button>
          {/if}
        </li>
      {/each}
      </ul>
    {/if}
  </div>

  <ListToolbar left={leftActions} right={rightActions}>
    {#snippet middle()}
      <FilterReveal bind:value={filter} />
    {/snippet}
  </ListToolbar>
</div>

{#if adding}
  <AddSourceModal canvas={null} scene={currentScene} onCreated={onSourceCreated} onClose={() => (adding = false)} />
{/if}

{#if propsForSource}
  <PropertiesModal
    kind="source"
    ref={propsForSource}
    title={"Properties — " + propsForSource}
    onClose={() => (propsForSource = null)}
  />
{/if}

{#if menu}
  <ContextMenu x={menu.x} y={menu.y} items={menu.items} onClose={() => (menu = null)} />
{/if}

<style>
  /* Sources rows are one row shorter than the shared default (5px 7px). */
  .dock-row {
    padding: 4px 7px;
  }
  /* Drag-reorder drop indicator. Outline avoids layout shift and the inline
     box-shadow the color tag already uses. */
  .dock-row.dropTarget {
    outline: var(--border-weight) solid var(--color-accent);
    outline-offset: -1px;
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
  /* Scroll region above the pinned bottom toolbar, so the toolbar stays at the
     dock's foot even when the list is empty or short. */
  .dock-fill {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
    overflow: auto;
  }
</style>
