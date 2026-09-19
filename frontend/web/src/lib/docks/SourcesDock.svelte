<script lang="ts">
  import { onMount, onDestroy } from "svelte";
  import {
    obs,
    type SceneItem,
    type SceneItemRef,
    type ReorderDirection,
    type PreviewSelectParams,
    type SceneItemsSetCollapsedParams,
  } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { defaultCanvas } from "$lib/docks/defaultCanvasStore.svelte";
  import AddSourceModal from "$lib/dialogs/add-source/AddSourceModal.svelte";
  import PropertiesModal from "$lib/properties/PropertiesModal.svelte";
  import { suspendPreview } from "$lib/stores/previewGate.svelte";
  import ContextMenu, { type ContextMenuState } from "$lib/menus/ContextMenu.svelte";
  import { clipboard } from "$lib/stores/clipboardStore.svelte";
  import { copyItem, pasteReference, pasteDuplicate } from "$lib/stores/clipboardItemState";
  import { sourceSelection } from "$lib/stores/sourceSelectionStore.svelte";
  import { activeSurface } from "$lib/stores/activeSurfaceStore.svelte";
  import { dockAction } from "$lib/stores/dockActionSignal.svelte";
  import { openFilters } from "$lib/dialogs/filterDialogOpener.svelte";
  import { DockError } from "$lib/docking/dockError.svelte";
  import { RequestGuard } from "$lib/utils/requestGuard";
  import { transformMenu } from "$lib/menus/transformMenu";
  import { scaleFilterMenu } from "$lib/menus/scaleFilterMenu";
  import { blendModeMenu, blendMethodMenu } from "$lib/menus/blendMenu";
  import { showTransitionMenu, hideTransitionMenu, transitionTypes } from "$lib/menus/transitionMenu";
  import { deinterlaceMenu } from "$lib/menus/deinterlaceMenu";
  import { colorMenu } from "$lib/menus/colorMenu";
  import type { DeinterlaceMode, DeinterlaceFieldOrder } from "$lib/api/bridge";
  import SourceTree from "$lib/docking/SourceTree.svelte";
  import { expandEnteredGroup, siblingPosition, visibleRows } from "$lib/docking/sourceTree";
  import { PendingRename } from "$lib/docking/pendingRename.svelte";
  import { itemTarget, sameItem, toRef } from "$lib/utils/sceneItemRef";
  import ListToolbar, { type ToolAction } from "$lib/docking/ListToolbar.svelte";
  import FilterReveal from "$lib/docking/FilterReveal.svelte";

  let {}: Record<string, unknown> = $props();

  // Identifies this dock's claim on the active surface. Not the SourceSelection instance:
  // the Scenes dock drives the same `sourceSelection` singleton, so only a per-component
  // token can tell the two claims apart.
  const surfaceOwner = Symbol("SourcesDock");

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
  const rows = $derived(visibleRows(items, filter));
  let loaded = $state(false);
  const dockError = new DockError();
  let propsForSource = $state<string | null>(null);
  let adding = $state(false);
  let renaming = $state<SceneItemRef | null>(null);
  let renameTo = $state("");
  let menu = $state<ContextMenuState | null>(null);

  // The bottom toolbar acts on the selected row (OBS list convention). Reorder is
  // indexed against the full, unfiltered list the row is ordered within (the scene's
  // items, or its group's children) so up/down disable at the ends.
  const selectedItem = $derived(sourceSelection.item);
  const selectedPos = $derived(selectedItem ? siblingPosition(items, selectedItem) : { index: -1, count: 0 });

  // Item calls are addressed through here, so the owning group rides along with the id.
  const at = (item: SceneItemRef) => itemTarget({ scene: currentScene }, item);

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
      disabled: filtering || selectedPos.index <= 0,
      onClick: () => selectedItem && void reorder(selectedItem, "up"),
    },
    {
      icon: "down",
      title: "Move down",
      disabled: filtering || selectedPos.index < 0 || selectedPos.index >= selectedPos.count - 1,
      onClick: () => selectedItem && void reorder(selectedItem, "down"),
    },
  ]);

  const report = dockError.report;

  function openProperties(item: SceneItem) {
    if (item.source) {
      propsForSource = item.source;
    }
  }

  // The tree changed the selection (a click or a key, plain or with a modifier). Push the
  // resulting set, focused row last, to the native preview, which draws the same selection
  // this list shows, and claim the surface, because a modifier click is still the user
  // working here and the app-level shortcuts have to follow them here.
  function onTreeSelect() {
    activeSurface.claimSource(surfaceOwner, null, sourceSelection);
    sourceSelection.pushToPreview((refs) => {
      const params: PreviewSelectParams = { scene: currentScene, refs };
      return obs.call("preview.select", params);
    });
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
        // The whole set, so a preview multi-select (Ctrl-click or a rubber band) lands
        // here as the set it is rather than as its focused item alone.
        activeSurface.claimSource(surfaceOwner, null, sourceSelection);
        sourceSelection.adoptPreview(p, items);
        // Drilling into a group in the preview picks from its children, so show them.
        void expandEnteredGroup(items, p.enteredGroup, setCollapsed);
      }
    });
  });

  // Request-generation guard: a fast scene switch must not let a slow prior list
  // response overwrite the newer scene's sources.
  const loadGuard = new RequestGuard();
  async function load() {
    if (!currentScene) {
      items = [];
      loaded = true;
      return;
    }
    const current = loadGuard.claim();
    dockError.clear();
    try {
      const list = await obs.call("sceneItems.list", { scene: currentScene });
      if (!current()) {
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
    activeSurface.release(surfaceOwner);
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
      const it = items.find((i) => sameItem(i, { id: created.id, group: null }));
      if (it) {
        activeSurface.claimSource(surfaceOwner, null, sourceSelection);
        sourceSelection.selectOne(it);
      }
    });
  }

  async function toggleVisible(item: SceneItem) {
    try {
      await obs.call("sceneItems.setVisible", { ...at(item), visible: !item.visible });
    } catch (e) {
      report(e);
    }
  }

  async function toggleLocked(item: SceneItem) {
    try {
      await obs.call("sceneItems.setLocked", { ...at(item), locked: !item.locked });
    } catch (e) {
      report(e);
    }
  }

  async function reorder(item: SceneItem, direction: ReorderDirection) {
    try {
      await obs.call("sceneItems.reorder", { ...at(item), direction });
    } catch (e) {
      report(e);
    }
  }

  // Drag-to-reorder (SourceTree): `to` is the drop row's top-first index within the
  // dragged item's own owner, the same order sceneItems.list returns; the bridge moves the
  // item there as one undo action, the same as the up/down buttons.
  async function reorderTo(item: SceneItem, to: number) {
    try {
      await obs.call("sceneItems.reorder", { ...at(item), to });
    } catch (e) {
      report(e);
    }
  }

  // Collapse state is saved on the group item and read back from the list it reloads.
  // Resolves to whether the host took it.
  async function setCollapsed(item: SceneItem, collapsed: boolean): Promise<boolean> {
    try {
      const params: SceneItemsSetCollapsedParams = { ...at(item), collapsed };
      await obs.call("sceneItems.setCollapsed", params);
      return true;
    } catch (e) {
      report(e);
      return false;
    }
  }

  async function remove(item: SceneItem) {
    try {
      await obs.call("sceneItems.remove", at(item));
    } catch (e) {
      report(e);
    }
  }

  // Batch remove: loop the existing single-item bridge remove once per selected ref (no
  // new bridge method; each removal stays independently undoable). Falls back to the
  // primary row when nothing is in the set. Snapshot the refs first — the removals fire
  // reload events that shrink the set mid-loop.
  async function removeSelected() {
    const refs = sourceSelection.size > 0 ? sourceSelection.removalRefs : selectedItem ? [selectedItem] : [];
    for (const ref of refs) {
      try {
        await obs.call("sceneItems.remove", at(ref));
      } catch (e) {
        report(e);
      }
    }
  }

  function beginRename(item: SceneItem) {
    pendingRename.clear();
    renaming = toRef(item);
    renameTo = item.source ?? "";
  }

  // Holds a rename for a child of a collapsed group while the group expands.
  const pendingRename = new PendingRename({
    items: () => items,
    rows: () => rows,
    filtering: () => filtering,
    scene: () => currentScene,
    begin: beginRename,
    expand: (group) => setCollapsed(group, false),
  });

  // App-level F2 (source target) opens the row's existing inline editor.
  $effect(() => {
    const p = dockAction.pending;
    if (p?.canvas === null && p.action.kind === "renameSource") {
      pendingRename.serve(p.action.ref, () => dockAction.consume(p.seq));
    }
  });

  async function commitRename() {
    const ref = renaming;
    const name = renameTo.trim();
    renaming = null;
    if (ref === null || !name) {
      return;
    }
    try {
      await obs.call("sources.rename", { ...at(ref), name });
    } catch (e) {
      report(e);
    }
  }

  // ---- clipboard actions (copy/paste/duplicate/filters/transform/group) ------
  // All target the global channel-0 path (no canvas); paste lands in currentScene.
  function copySource(item: SceneItem) {
    void copyItem(at(item), item);
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
      await obs.call("sources.duplicate", at(item));
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
      clipboard.transform = await obs.call("sceneItems.getTransform", at(item));
    } catch (e) {
      report(e);
    }
  }

  async function pasteTransform(item: SceneItem) {
    if (!clipboard.transform) {
      return;
    }
    try {
      await obs.call("sceneItems.setTransform", { ...at(item), transform: clipboard.transform });
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
      await obs.call("sceneItems.ungroup", at(item));
    } catch (e) {
      report(e);
    }
  }

  // Deinterlacing lives on the source (not the scene item), so it isn't in the row
  // data; fetch it just-in-time when the menu opens. A failed read still yields a
  // menu, but the checked entry is then a guess rather than the source's real
  // state — report it so the discrepancy isn't silent.
  async function fetchDeint(source: string): Promise<{ mode: DeinterlaceMode; fieldOrder: DeinterlaceFieldOrder }> {
    try {
      return await obs.call("sources.getDeinterlace", { source });
    } catch (e) {
      report(e);
      return { mode: "disable", fieldOrder: "top" };
    }
  }

  async function openMenu(e: MouseEvent, item: SceneItem) {
    e.preventDefault();
    const pos = siblingPosition(items, item);
    const x = e.clientX;
    const y = e.clientY;
    const deint = item.source ? await fetchDeint(item.source) : { mode: "disable" as const, fieldOrder: "top" as const };
    const transitionTypeList = await transitionTypes().catch(() => []);
    // Right-clicking a row that is part of a 2+ selection acts on the whole set; a
    // right-click on a single (or unselected) row stays single.
    const inMulti = sourceSelection.has(item) && sourceSelection.size >= 2;
    menu = {
      x,
      y,
      items: [
        { label: "Properties", action: () => openProperties(item) },
        { label: "Filters", disabled: !item.source, action: () => item.source && openFilters(item.source) },
        ...(item.interactive && item.source
          ? [{ label: "Interact", action: () => void obs.call("sources.interact", { source: item.source }).catch(report) }]
          : []),
        transformMenu(at(item), item.source ?? "(unnamed)"),
        { label: "Rename", action: () => beginRename(item) },
        scaleFilterMenu(item.scaleFilter, (filter) =>
          void obs.call("sceneItems.setScaleFilter", { ...at(item), filter }).catch(report),
        ),
        blendModeMenu(item.blendMode, (mode) =>
          void obs.call("sceneItems.setBlendingMode", { ...at(item), mode }).catch(report),
        ),
        blendMethodMenu(item.blendMethod, (method) =>
          void obs.call("sceneItems.setBlendingMethod", { ...at(item), method }).catch(report),
        ),
        showTransitionMenu(
          item.showTransition,
          transitionTypeList,
          (type) =>
            void obs
              .call("sceneItems.setShowTransition", {
                ...at(item),
                transition: type,
                duration: item.showTransition?.duration ?? 300,
              })
              .catch(report),
          (duration) =>
            void obs
              .call("sceneItems.setShowTransition", {
                ...at(item),
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
                ...at(item),
                transition: type,
                duration: item.hideTransition?.duration ?? 300,
              })
              .catch(report),
          (duration) =>
            void obs
              .call("sceneItems.setHideTransition", {
                ...at(item),
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
          void obs.call("sceneItems.setColor", { ...at(item), color }).catch(report),
        ),
        {
          label: "Screenshot",
          disabled: !item.source,
          action: () => void obs.call("screenshot.takeSource", at(item)).catch(report),
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
        // A group's child is never a group itself (groups cannot nest), and Group's `ids`
        // addresses top-level items only.
        { label: "Group", disabled: item.group !== null, action: () => void groupItem(item) },
        { label: "Ungroup", disabled: item.group !== null, action: () => void ungroupItem(item) },
        null,
        { label: item.visible ? "Hide" : "Show", action: () => void toggleVisible(item) },
        { label: item.locked ? "Unlock" : "Lock", action: () => void toggleLocked(item) },
        null,
        { label: "Move Up", disabled: filtering || pos.index === 0, action: () => void reorder(item, "up") },
        {
          label: "Move Down",
          disabled: filtering || pos.index === pos.count - 1,
          action: () => void reorder(item, "down"),
        },
        { label: "Move to Top", disabled: filtering || pos.index === 0, action: () => void reorder(item, "top") },
        {
          label: "Move to Bottom",
          disabled: filtering || pos.index === pos.count - 1,
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
    {#if dockError.message}
      <p class="dock-msg err" role="alert">{dockError.message}</p>
    {:else if !currentScene}
      <p class="dock-msg">No scene selected</p>
    {:else if !loaded}
      <p class="dock-msg">Loading…</p>
    {:else if items.length === 0}
      <p class="dock-msg">No sources</p>
    {:else if rows.length === 0}
      <p class="dock-msg">No matches</p>
    {:else}
      <SourceTree
        {rows}
        selection={sourceSelection}
        variant="dock"
        label="Sources"
        {filtering}
        {renaming}
        bind:renameTo
        onSelect={onTreeSelect}
        onRenameCommit={() => void commitRename()}
        onRenameCancel={() => (renaming = null)}
        onToggleVisible={(item) => void toggleVisible(item)}
        onToggleLocked={(item) => void toggleLocked(item)}
        onOpenProperties={openProperties}
        onContextMenu={(e, item) => void openMenu(e, item)}
        onReorder={(item, to) => void reorderTo(item, to)}
        onSetCollapsed={setCollapsed}
      />
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
