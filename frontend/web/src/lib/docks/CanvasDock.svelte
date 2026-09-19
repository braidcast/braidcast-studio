<script lang="ts">
  import { onMount, onDestroy } from "svelte";
  import {
    obs,
    type SceneInfo,
    type SceneItem,
    type SceneItemRef,
    type ReorderDirection,
    type PreviewSelectParams,
    type SceneItemsSetCollapsedParams,
    type MultistreamState,
    type SceneLinkInfo,
    type PreviewHitTarget,
  } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import Button from "$lib/ui/Button.svelte";
  import { selectOnMount } from "$lib/utils/focusActions";
  import { clamp } from "$lib/utils/clamp";
  import { suspendPreview } from "$lib/stores/previewGate.svelte";
  import { PreviewFreeze } from "$lib/stores/previewFreeze.svelte";
import { dockLayout } from "$lib/docking/dockLayoutSignal.svelte";
  import { WINDOW_ID } from "$lib/utils/windowContext";
  import {
    reportPreviewRect,
    hidePreview as hidePreviewSurface,
    destroyPreview,
    mapOverlayCursor,
    fetchPreviewView,
    previewViewMenuItems,
    forwardPreviewWheel,
    syncPreviewGate,
    previewTarget,
    type PreviewSurfaceDock,
  } from "$lib/docking/previewSurface";
  import { usePreviewPointerGuard } from "$lib/docking/previewPointerGuard";
  import { isPreviewDisabled, setPreviewDisabled } from "$lib/docking/previewDisabledStore.svelte";
  import { DockError } from "$lib/docking/dockError.svelte";
  import ContextMenu, { type ContextMenuItems, type ContextMenuState } from "$lib/menus/ContextMenu.svelte";
  import { clipboard } from "$lib/stores/clipboardStore.svelte";
  import { copyItem, pasteReference, pasteDuplicate } from "$lib/stores/clipboardItemState";
  import { SourceSelection } from "$lib/stores/sourceSelectionStore.svelte";
  import { activeSurface } from "$lib/stores/activeSurfaceStore.svelte";
  import { dockAction } from "$lib/stores/dockActionSignal.svelte";
  import { openFilters } from "$lib/dialogs/filterDialogOpener.svelte";
  import { transformMenu } from "$lib/menus/transformMenu";
  import { scaleFilterMenu } from "$lib/menus/scaleFilterMenu";
  import { blendModeMenu, blendMethodMenu } from "$lib/menus/blendMenu";
  import { showTransitionMenu, hideTransitionMenu, transitionTypes } from "$lib/menus/transitionMenu";
  import { deinterlaceMenu } from "$lib/menus/deinterlaceMenu";
  import { colorMenu } from "$lib/menus/colorMenu";
  import type { DeinterlaceMode, DeinterlaceFieldOrder, TransitionType } from "$lib/api/bridge";
  import { defaultCanvas } from "$lib/docks/defaultCanvasStore.svelte";
  import { canvasStore } from "$lib/stores/canvasStore.svelte";
  import { callOrToast, renamedSuffix } from "$lib/utils/callToast";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import AddSourceModal from "$lib/dialogs/add-source/AddSourceModal.svelte";
  import PropertiesModal from "$lib/properties/PropertiesModal.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import SourceTree from "$lib/docking/SourceTree.svelte";
  import { expandEnteredGroup, siblingPosition, visibleRows } from "$lib/docking/sourceTree";
  import { PendingRename } from "$lib/docking/pendingRename.svelte";
  import { itemTarget, sameItem, toRef } from "$lib/utils/sceneItemRef";
  import ListToolbar, { type ToolAction } from "$lib/docking/ListToolbar.svelte";
  import FilterReveal from "$lib/docking/FilterReveal.svelte";
  import Splitter from "$lib/docking/Splitter.svelte";
  import { getPaneSizes, setEmbedH, setScenesW } from "$lib/docking/canvasPaneSizes";
  import { multistreamStatusStore } from "$lib/stores/multistreamStatusStore.svelte";
  import {
    SceneDragReorder,
    sceneMoveActions,
    sceneOrderMenuChildren,
    type SceneOrderTarget,
  } from "$lib/utils/sceneReorder.svelte";

  // A composite, inseparable dock for one NON-DEFAULT canvas (hierarchy-model.html
  // §1 right column): an inline preview + this canvas's own scenes + its own
  // sources, all scoped to the canvas uuid (the additional-canvas path — every
  // bridge call carries { canvas: canvasUuid }). The whole dock is the floatable
  // unit; its internals are NOT separable into global docks. Output-gated: App only
  // mounts this dock while >=1 enabled output binds the canvas.
  interface Props {
    canvasUuid: string;
    canvasName: string;
  }
  let { canvasUuid, canvasName }: Props = $props();

  // Identifies this dock's claim on the active surface. A per-component token rather than
  // the SourceSelection instance, so a dock that shares a selection with another (the
  // Default canvas's Scenes + Sources pair) can still release only its own claim.
  const surfaceOwner = Symbol("CanvasDock");

  const dockError = new DockError();
  const report = dockError.report;

  // One context menu for the whole dock (scene rows, source rows, and the preview
  // all use it). `suspendOverlay` is set only by the preview menu: it opens over
  // the native overlay and must blank it; row menus open in the list area and must
  // not (blanking would flash the preview off for no reason).
  let menu = $state<(ContextMenuState & { suspendOverlay?: boolean }) | null>(null);

  // ---- inline preview region (native overlay scoped to this canvas) -----------
  let previewEl = $state<HTMLElement | undefined>();
  // True once we've asserted a valid rect and the native surface should be painting;
  // drives hiding the DOM stage outline so it doesn't linger as a ghost frame over
  // the live video (it re-appears as the placeholder frame whenever we hide).
  let surfaceActive = $state(false);

  // Coalesce rect recomputes to one per frame. ResizeObserver can fire a burst per
  // drag frame; flooding preview.setRect makes the async native reposition trail
  // (the drift). One send per frame with the latest rect keeps the surface tracking.
  let rectRaf = 0;
  function scheduleRect() {
    if (rectRaf) {
      return;
    }
    rectRaf = requestAnimationFrame(() => {
      rectRaf = 0;
      reportRect();
    });
  }

  // A disabled preview stays hidden over its placeholder; it is already destroyed then
  // (see disablePreview below), so the hide is a harmless idempotent no-op. `shown`
  // mirrors each report into surfaceActive so the DOM stage outline drops only while the
  // native surface owns the stage.
  const surfaceDock: PreviewSurfaceDock = {
    get canvasUuid() {
      return canvasUuid;
    },
    blocked: () => isPreviewDisabled(canvasUuid),
    hide: () => hidePreview(),
    shown: (shown) => {
      surfaceActive = shown;
    },
  };

  function reportRect(): boolean {
    return reportPreviewRect(previewEl, surfaceDock);
  }
  function hidePreview() {
    surfaceActive = false;
    hidePreviewSurface(canvasUuid);
  }

  // OBS parity: "Disable Preview" stops rendering the native surface to save GPU.
  // Destroying (not hiding) is what actually matters -- the native preview surface
  // holds a main-render ref that keeps this canvas compositing every frame; only
  // releasing the surface via destroy() drops that ref and lets it go idle.
  function disablePreview() {
    setPreviewDisabled(canvasUuid, true);
    destroyPreview(canvasUuid);
  }

  // Re-enable: flip the flag, then measure immediately so the surface repaints on
  // this frame instead of waiting for the next resize/layout event to trigger it.
  function enablePreview() {
    setPreviewDisabled(canvasUuid, false);
    requestAnimationFrame(reportRect);
  }

  // ---- canvas geometry (stage res chip + aspect) -----------------------------
  // Display-only read: the mock stage needs the canvas resolution (res chip) and
  // aspect (9:16 vertical vs 16:9). CanvasDock receives only uuid+name, so look
  // this canvas up in canvas.list and refresh on canvas.changed. Nothing in the
  // scene/source/preview path depends on it.
  let canvasInfo = $derived(canvasStore.byUuid(canvasUuid) ?? null);
  let vertical = $derived(!!canvasInfo && canvasInfo.outputHeight > canvasInfo.outputWidth);
  let resText = $derived(canvasInfo ? canvasInfo.outputWidth + " × " + canvasInfo.outputHeight : "");

  // ---- scenes (this canvas's own scene list) ---------------------------------
  let scenes = $state<SceneInfo[]>([]);
  let currentScene = $state<string | null>(null);
  let loaded = $state(false);
  // Whether Remove can act at all. One predicate behind the toolbar button, the context
  // menu entry and the claim the app-level Delete reads, so the keyboard cannot offer a
  // removal this dock's own chrome disables.
  let canRemoveScene = $derived(scenes.length > 1);

  async function loadScenes() {
    try {
      const list = await obs.call("scenes.list", { canvas: canvasUuid });
      scenes = list;
      currentScene = list.find((s) => s.current)?.name ?? null;
      dockError.clear();
    } catch (e) {
      report(e);
    } finally {
      loaded = true;
    }
  }
  // Claim before the unchanged-scene bail: clicking a row that is already current still
  // hands this canvas the app-level shortcuts, and still drops the source selection that
  // nothing else would clear (Delete would otherwise remove that stale source).
  function setCurrentScene(name: string) {
    activeSurface.claimScene(surfaceOwner, canvasUuid, selection, name, () => canRemoveScene);
    if (name === currentScene) {
      return;
    }
    obs.call("scenes.setCurrent", { canvas: canvasUuid, name }).catch(report);
  }

  // ---- scene links (which Default scenes each of this canvas's scenes follows) -
  // Scene links whose canvas is THIS dock's canvas. Drives the row 🔗 indicator
  // and the submenu's checked state.
  let myLinks = $state<SceneLinkInfo[]>([]);

  async function loadLinks() {
    try {
      const res = await obs.call("sceneLink.list");
      myLinks = res.links.filter((l) => l.canvas === canvasUuid);
    } catch {
      myLinks = [];
    }
  }

  // canvasSceneName -> main scene names it follows (resolved, non-empty only).
  let linksByCanvasScene = $derived.by(() => {
    const m = new Map<string, string[]>();
    for (const l of myLinks) {
      if (!l.canvasSceneName) continue;
      const arr = m.get(l.canvasSceneName) ?? [];
      if (l.mainSceneName) arr.push(l.mainSceneName);
      m.set(l.canvasSceneName, arr);
    }
    return m;
  });

  function isLinked(canvasSceneName: string, mainSceneName: string): boolean {
    return myLinks.some((l) => l.canvasSceneName === canvasSceneName && l.mainSceneName === mainSceneName);
  }

  function toggleLink(canvasSceneName: string, mainSceneName: string) {
    const method = isLinked(canvasSceneName, mainSceneName) ? "sceneLink.clear" : "sceneLink.set";
    const params =
      method === "sceneLink.set"
        ? { mainScene: mainSceneName, canvas: canvasUuid, canvasScene: canvasSceneName }
        : { mainScene: mainSceneName, canvas: canvasUuid };
    obs.call(method, params).catch(report);
  }

  // ---- scene rename / remove (scoped to this canvas) -------------------------
  let renamingScene = $state<string | null>(null);
  let renameSceneTo = $state("");

  function beginRenameScene(name: string) {
    renamingScene = name;
    renameSceneTo = name;
  }
  function renameScene(from: string, to: string) {
    obs.call("scenes.rename", { canvas: canvasUuid, from, to }).catch(report);
  }
  function commitRenameScene() {
    const from = renamingScene;
    const to = renameSceneTo.trim();
    renamingScene = null;
    if (!from || !to || to === from) {
      return;
    }
    renameScene(from, to);
  }
  function onRenameSceneKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      commitRenameScene();
    } else if (e.key === "Escape") {
      renamingScene = null;
    }
  }
  function removeScene(name: string) {
    obs.call("scenes.remove", { canvas: canvasUuid, name }).catch(report);
  }

  // ---- add scene (inline, scoped to this canvas) -----------------------------
  let addingScene = $state(false);
  let newSceneName = $state("");
  function beginAddScene() {
    addingScene = true;
    newSceneName = "";
  }
  async function commitAddScene() {
    const name = newSceneName.trim();
    addingScene = false;
    if (!name) {
      return;
    }
    try {
      await obs.call("scenes.create", { name, canvas: canvasUuid });
      await obs.call("scenes.setCurrent", { canvas: canvasUuid, name });
    } catch (e) {
      report(e);
    }
  }
  function onAddSceneKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      void commitAddScene();
    } else if (e.key === "Escape") {
      addingScene = false;
    }
  }

  // ---- scene name filter (behind the toolbar reveal) -------------------------
  // Reorder (buttons, menu, drag) is disabled while filtering since indices only
  // make sense against the full, unfiltered ordering (mirrors ScenesDock).
  let sceneFilter = $state("");
  let sceneFiltering = $derived(sceneFilter.trim().length > 0);
  let filteredScenes = $derived(
    sceneFiltering
      ? scenes.filter((s) => s.name.toLowerCase().includes(sceneFilter.trim().toLowerCase()))
      : scenes,
  );

  // ---- scene reorder (this canvas's own persisted order) ---------------------
  // Every call carries `canvas`, so the bridge reorders within THIS canvas's scene
  // order and never the Default canvas's. `to` is the drop row's top-first index,
  // the same order this list renders and scenes.list returns.
  let currentSceneIdx = $derived(currentScene === null ? -1 : scenes.findIndex((s) => s.name === currentScene));

  async function reorderScene(name: string, direction: ReorderDirection) {
    try {
      await obs.call("scenes.reorder", { name, canvas: canvasUuid, direction });
    } catch (e) {
      report(e);
    }
  }

  async function reorderSceneTo(name: string, to: number) {
    try {
      await obs.call("scenes.reorder", { name, canvas: canvasUuid, to });
    } catch (e) {
      report(e);
    }
  }

  const sceneDnd = new SceneDragReorder({
    move: (name, to) => void reorderSceneTo(name, to),
    indexOf: (name) => scenes.findIndex((s) => s.name === name),
    disabled: () => sceneFiltering,
  });

  // The toolbar acts on the current scene, the menu on the right-clicked one; both
  // index against the full, unfiltered list.
  function sceneOrderTarget(idx: number, move: (direction: ReorderDirection) => void): SceneOrderTarget {
    return { idx, count: scenes.length, disabled: sceneFiltering, move };
  }

  // Duplicates a scene within THIS canvas (shared source refs, matching OBS's own
  // "Duplicate Scene"). See scenes.duplicate in bridge.ts.
  async function duplicateScene(sceneName: string) {
    const r = await callOrToast("scenes.duplicate", { name: sceneName, canvas: canvasUuid }, "Duplicate failed");
    if (r) {
      showToast(`Duplicated "${sceneName}"${renamedSuffix(sceneName, r.name)}`, r.name);
    }
  }

  // Duplicates a scene from THIS canvas onto another canvas (a deep copy, unlike
  // the same-canvas sceneItems.duplicate below which is a ref duplicate). See
  // scenes.duplicateToCanvas in bridge.ts.
  async function duplicateSceneToCanvas(sceneName: string, destUuid: string) {
    const r = await callOrToast(
      "scenes.duplicateToCanvas",
      { name: sceneName, canvas: canvasUuid, destCanvas: destUuid },
      "Duplicate failed",
    );
    if (r) {
      const destName = canvasStore.byUuid(destUuid)?.name;
      const to = destName ? ` to "${destName}"` : "";
      showToast(`Duplicated "${sceneName}"${to}${renamedSuffix(sceneName, r.name)}`, r.name);
    }
  }

  async function copySceneFilters(name: string) {
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
    try {
      await obs.call("filters.pasteChain", { source: name, filters: clipboard.filters });
    } catch (e) {
      report(e);
    }
  }

  function openSceneMenu(e: MouseEvent, name: string) {
    e.preventDefault();
    const linkChildren =
      defaultCanvas.scenes.length === 0
        ? [{ label: "(no main scenes)", disabled: true }]
        : defaultCanvas.scenes.map((ms) => ({
            label: ms.name,
            checked: isLinked(name, ms.name),
            action: () => toggleLink(name, ms.name),
          }));
    const otherCanvases = canvasStore.canvases.filter((c) => c.uuid !== canvasUuid);
    const duplicateChildren =
      otherCanvases.length === 0
        ? [{ label: "(no other canvases)", disabled: true }]
        : otherCanvases.map((c) => ({
            label: c.name,
            action: () => void duplicateSceneToCanvas(name, c.uuid),
          }));
    const idx = scenes.findIndex((s) => s.name === name);
    const orderChildren = sceneOrderMenuChildren(sceneOrderTarget(idx, (d) => void reorderScene(name, d)));
    menu = {
      x: e.clientX,
      y: e.clientY,
      items: [
        { label: "Rename", action: () => beginRenameScene(name) },
        { label: "Filters", action: () => openFilters(name) },
        { label: "Link to", children: linkChildren },
        { label: "Duplicate", action: () => void duplicateScene(name) },
        { label: "Duplicate to canvas", children: duplicateChildren },
        { label: "Order", children: orderChildren },
        // A scene is a source, so its screenshot reuses screenshot.takeSource (the
        // per-source path), targeting the scene by name instead of a scene-item id.
        {
          label: "Screenshot Scene",
          action: () => void obs.call("screenshot.takeSource", { canvas: canvasUuid, scene: name }).catch(report),
        },
        null,
        { label: "Copy Filters", action: () => void copySceneFilters(name) },
        { label: "Paste Filters", disabled: !clipboard.filters, action: () => void pasteSceneFilters(name) },
        null,
        { label: "Remove", danger: true, disabled: !canRemoveScene, action: () => removeScene(name) },
      ],
    };
  }

  // ---- sources (current scene of this canvas) --------------------------------
  let items = $state<SceneItem[]>([]);
  // Per-canvas source selection with its own instance — the global sourceSelection
  // singleton is Default-canvas only. Holds the multi-select set for this canvas's
  // scene; `.item` is the focused row the toolbar acts on.
  const selection = new SourceSelection();

  // The Sources toolbar acts on the primary row (the OBS list convention); these
  // derive the target + its index so delete/move can disable when there is none.
  let selectedItem = $derived(selection.item);
  let selectedPos = $derived(selectedItem ? siblingPosition(items, selectedItem) : { index: -1, count: 0 });

  // Item calls are addressed through here, so the owning group rides along with the id.
  const at = (item: SceneItemRef) => itemTarget({ canvas: canvasUuid, scene: currentScene }, item);

  // ---- source name filter (behind the toolbar reveal) ------------------------
  let sourceFilter = $state("");
  let sourceFiltering = $derived(sourceFilter.trim().length > 0);
  let sourceRows = $derived(visibleRows(items, sourceFilter));

  // Request-generation guard: a fast scene switch must not let a slow prior list
  // response overwrite the newer scene's sources.
  let itemsSeq = 0;
  async function loadItems() {
    if (!currentScene) {
      items = [];
      return;
    }
    const mine = ++itemsSeq;
    try {
      const list = await obs.call("sceneItems.list", { canvas: canvasUuid, scene: currentScene });
      if (mine !== itemsSeq) {
        return;
      }
      items = list;
    } catch (e) {
      report(e);
    }
  }
  // The tree changed the selection (a click or a key, plain or with a modifier). Push the
  // resulting set, focused row last, to the native preview, which draws the same selection
  // this list shows.
  function onTreeSelect() {
    activeSurface.claimSource(surfaceOwner, canvasUuid, selection);
    selection.pushToPreview((refs) => {
      const params: PreviewSelectParams = { ...previewTarget(canvasUuid), scene: currentScene, refs };
      return obs.call("preview.select", params);
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
  // primary row when nothing is in the set. Snapshot the refs — the removals fire reload
  // events that shrink the set mid-loop.
  async function removeSelected() {
    const refs = selection.size > 0 ? selection.removalRefs : selectedItem ? [selectedItem] : [];
    for (const ref of refs) {
      try {
        await obs.call("sceneItems.remove", at(ref));
      } catch (e) {
        report(e);
      }
    }
  }

  // ---- add source / properties (scoped to this canvas's current scene) -------
  let addingSource = $state(false);
  let propsForSource = $state<string | null>(null);
  function onSourceCreated(created: { id: number; source: string }) {
    addingSource = false;
    propsForSource = created.source;
    void loadItems().then(() => {
      const it = items.find((i) => sameItem(i, { id: created.id, group: null }));
      if (it) {
        activeSurface.claimSource(surfaceOwner, canvasUuid, selection);
        selection.selectOne(it);
      }
    });
  }
  function openProperties(item: SceneItem) {
    if (item.source) {
      propsForSource = item.source;
    }
  }

  // ---- source rename (scoped to this canvas's current scene) -----------------
  let renaming = $state<SceneItemRef | null>(null);
  let renameTo = $state("");

  // Holds a rename for a child of a collapsed group while the group expands.
  const pendingRename = new PendingRename({
    items: () => items,
    rows: () => sourceRows,
    filtering: () => sourceFiltering,
    scene: () => currentScene,
    begin: beginRenameSource,
    expand: (group) => setCollapsed(group, false),
  });

  function beginRenameSource(item: SceneItem) {
    pendingRename.clear();
    renaming = toRef(item);
    renameTo = item.source ?? "";
  }
  async function commitRenameSource() {
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
  // Every call carries this canvas's uuid (the additional-canvas path); paste
  // lands in this canvas's current scene. Filter copy/paste keys off the source
  // name and so shares the same clipboard slots as the global SourcesDock.
  function copySource(item: SceneItem) {
    void copyItem(at(item), item);
  }
  async function pasteSource() {
    try {
      await pasteReference({ canvas: canvasUuid, scene: currentScene });
    } catch (e) {
      report(e);
    }
  }
  async function pasteDuplicateSource() {
    try {
      await pasteDuplicate({ canvas: canvasUuid, scene: currentScene });
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
      await obs.call("sceneItems.group", { canvas: canvasUuid, scene: currentScene, ids: [item.id] });
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

  // Deinterlacing is a source-level property (no canvas), fetched just-in-time when
  // the menu opens. A failed read still yields a menu, but the checked entry is then
  // a guess rather than the source's real state — report it so the discrepancy isn't
  // silent.
  async function fetchDeint(source: string): Promise<{ mode: DeinterlaceMode; fieldOrder: DeinterlaceFieldOrder }> {
    try {
      return await obs.call("sources.getDeinterlace", { source });
    } catch (e) {
      report(e);
      return { mode: "disable", fieldOrder: "top" };
    }
  }

  async function openSourceMenu(e: MouseEvent, item: SceneItem) {
    e.preventDefault();
    const x = e.clientX;
    const y = e.clientY;
    const pos = siblingPosition(items, item);
    const deint = item.source ? await fetchDeint(item.source) : { mode: "disable" as const, fieldOrder: "top" as const };
    const transitionTypeList = await transitionTypes().catch(() => []);
    // Right-clicking a row that is part of a 2+ selection acts on the whole set; a
    // right-click on a single (or unselected) row stays single.
    const inMulti = selection.has(item) && selection.size >= 2;
    menu = {
      x,
      y,
      items: [
        { label: "Filters", disabled: !item.source, action: () => item.source && openFilters(item.source) },
        ...(item.interactive && item.source
          ? [{ label: "Interact", action: () => void obs.call("sources.interact", { source: item.source }).catch(report) }]
          : []),
        transformMenu(at(item), item.source ?? "(unnamed)"),
        { label: "Rename", action: () => beginRenameSource(item) },
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
        colorMenu(item.color, (color) => void obs.call("sceneItems.setColor", { ...at(item), color }).catch(report)),
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
        { label: "Move Up", disabled: pos.index === 0, action: () => void reorder(item, "up") },
        { label: "Move Down", disabled: pos.index === pos.count - 1, action: () => void reorder(item, "down") },
        { label: "Move to Top", disabled: pos.index === 0, action: () => void reorder(item, "top") },
        { label: "Move to Bottom", disabled: pos.index === pos.count - 1, action: () => void reorder(item, "bottom") },
        // Projector entries hidden pending the projector redesign.
        null,
        {
          label: inMulti ? `Remove ${selection.size} Items` : "Remove",
          danger: true,
          action: () => void (inMulti ? removeSelected() : remove(item)),
        },
      ],
    };
  }

  // ---- preview right-click menu (this canvas's hit scene item) ---------------
  // No Properties (matches the row menu, which omits it for additional-canvas
  // private sources). Every call carries this canvas's uuid + scene + item id.
  function buildPreviewItems(
    p: PreviewHitTarget,
    deint: { mode: DeinterlaceMode; fieldOrder: DeinterlaceFieldOrder },
    transitionTypeList: TransitionType[],
  ): ContextMenuItems {
    const call = (method: string, params: Record<string, unknown>) =>
      obs.call(method, { canvas: canvasUuid, scene: p.scene, id: p.id, ...params }).catch(report);
    const currentFilter = items.find((i) => i.id === p.id)?.scaleFilter ?? "disable";
    const currentBlendMode = items.find((i) => i.id === p.id)?.blendMode ?? "normal";
    const currentBlendMethod = items.find((i) => i.id === p.id)?.blendMethod ?? "default";
    const currentColor = items.find((i) => i.id === p.id)?.color ?? "";
    const currentShowTransition = items.find((i) => i.id === p.id)?.showTransition ?? null;
    const currentHideTransition = items.find((i) => i.id === p.id)?.hideTransition ?? null;
    return [
      ...(p.id != null
        ? [transformMenu({ canvas: canvasUuid, scene: p.scene, id: p.id, group: null }, p.source ?? "(unnamed)")]
        : []),
      scaleFilterMenu(currentFilter, (filter) => void call("sceneItems.setScaleFilter", { filter })),
      blendModeMenu(currentBlendMode, (mode) => void call("sceneItems.setBlendingMode", { mode })),
      blendMethodMenu(currentBlendMethod, (method) => void call("sceneItems.setBlendingMethod", { method })),
      showTransitionMenu(
        currentShowTransition,
        transitionTypeList,
        (type) =>
          void call("sceneItems.setShowTransition", { transition: type, duration: currentShowTransition?.duration ?? 300 }),
        (duration) =>
          void call("sceneItems.setShowTransition", { transition: currentShowTransition?.type ?? null, duration }),
      ),
      hideTransitionMenu(
        currentHideTransition,
        transitionTypeList,
        (type) =>
          void call("sceneItems.setHideTransition", { transition: type, duration: currentHideTransition?.duration ?? 300 }),
        (duration) =>
          void call("sceneItems.setHideTransition", { transition: currentHideTransition?.type ?? null, duration }),
      ),
      ...(p.source
        ? [
            deinterlaceMenu(
              deint.mode,
              deint.fieldOrder,
              (mode) => void obs.call("sources.setDeinterlace", { source: p.source, mode }).catch(report),
              (fieldOrder) => void obs.call("sources.setDeinterlace", { source: p.source, fieldOrder }).catch(report),
            ),
          ]
        : []),
      colorMenu(currentColor, (color) => void call("sceneItems.setColor", { color })),
      { label: "Screenshot", disabled: !p.source, action: () => void call("screenshot.takeSource", {}) },
      null,
      { label: p.visible ? "Hide" : "Show", action: () => void call("sceneItems.setVisible", { visible: !p.visible }) },
      { label: p.locked ? "Unlock" : "Lock", action: () => void call("sceneItems.setLocked", { locked: !p.locked }) },
      null,
      { label: "Move Up", action: () => void call("sceneItems.reorder", { direction: "up" }) },
      { label: "Move Down", action: () => void call("sceneItems.reorder", { direction: "down" }) },
      { label: "Move to Top", action: () => void call("sceneItems.reorder", { direction: "top" }) },
      { label: "Move to Bottom", action: () => void call("sceneItems.reorder", { direction: "bottom" }) },
      // Projector entries hidden pending the projector redesign.
      null,
      { label: "Remove", danger: true, action: () => void call("sceneItems.remove", {}) },
      null,
      { label: "Disable Preview", action: disablePreview },
    ];
  }

  // Empty-area menu (right-click with no source under the cursor). Add Source + Paste
  // reuse this canvas's add-source modal + clipboard paste; New Group creates an empty
  // group via sceneItems.createGroup in this canvas's current scene.
  function buildEmptyItems(): ContextMenuItems {
    return [
      { label: "Add Source", disabled: !currentScene, action: () => (addingSource = true) },
      {
        label: "New Group",
        disabled: !currentScene,
        action: () =>
          void obs.call("sceneItems.createGroup", { canvas: canvasUuid, scene: currentScene }).catch(report),
      },
      { label: "Paste", disabled: !clipboard.source, action: () => void pasteSource() },
      { label: "Paste (Duplicate)", disabled: !clipboard.source, action: () => void pasteDuplicateSource() },
    ];
  }

  // ---- live state for this canvas (drives the stage LIVE chip), off the shared status store ----
  let liveState = $derived.by<MultistreamState | "off">(() => {
    const mine = multistreamStatusStore.forCanvas(canvasUuid);
    return mine.length === 0 ? "off" : multistreamStatusStore.deriveOutputsState(mine);
  });
  // ---- toolbar actions (bottom bars, act on the selected row) ----------------
  let scenesLeft = $derived<ToolAction[]>([
    { icon: "plus", title: "Add scene", onClick: beginAddScene },
    {
      icon: "trash",
      title: "Delete scene",
      disabled: !currentScene || !canRemoveScene,
      onClick: () => currentScene && removeScene(currentScene),
    },
  ]);
  let scenesRight = $derived<ToolAction[]>(
    sceneMoveActions(sceneOrderTarget(currentSceneIdx, (d) => currentScene && void reorderScene(currentScene, d))),
  );
  let sourcesLeft = $derived<ToolAction[]>([
    { icon: "plus", title: "Add source", disabled: !currentScene, onClick: () => (addingSource = true) },
    { icon: "trash", title: "Delete source", disabled: !selectedItem, onClick: () => void removeSelected() },
    {
      icon: "gear",
      title: "Properties",
      disabled: !selectedItem?.source,
      onClick: () => selectedItem && openProperties(selectedItem),
    },
  ]);
  let sourcesRight = $derived<ToolAction[]>([
    {
      icon: "up",
      title: "Move up",
      disabled: sourceFiltering || selectedPos.index <= 0,
      onClick: () => selectedItem && void reorder(selectedItem, "up"),
    },
    {
      icon: "down",
      title: "Move down",
      disabled: sourceFiltering || selectedPos.index < 0 || selectedPos.index >= selectedPos.count - 1,
      onClick: () => selectedItem && void reorder(selectedItem, "down"),
    },
  ]);

  // ---- resizable sub-panes (persisted per canvas across remount) -------------
  // `embedH`/`scenesW` are px once dragged; null means "never dragged" so the CSS
  // fallbacks (154px / 42%) apply. On first drag we seed from the live measurement.
  // canvasUuid is fixed for a dock instance (the reconciler mounts one dock per
  // canvas), so reading it once to seed the persisted sizes is intentional.
  // svelte-ignore state_referenced_locally
  const seed = getPaneSizes(canvasUuid);
  let embedH = $state<number | null>(seed.embedH);
  let scenesW = $state<number | null>(seed.scenesW);
  let dockBodyEl = $state<HTMLElement | undefined>();
  let embedEl = $state<HTMLElement | undefined>();
  let scenesColEl = $state<HTMLElement | undefined>();

  function onEmbedDrag(dy: number) {
    const cur = embedH ?? embedEl?.getBoundingClientRect().height ?? 154;
    const dockH = dockBodyEl?.clientHeight ?? 600;
    // Dragging the divider up (dy<0) grows the embed; floor keeps the preview, cap
    // at 60% of the dock so the stage never collapses.
    const next = clamp(cur - dy, 120, dockH * 0.6);
    embedH = next;
    setEmbedH(canvasUuid, next);
  }
  function onScenesDrag(dx: number) {
    const cur = scenesW ?? scenesColEl?.getBoundingClientRect().width ?? 0;
    const totalW = embedEl?.clientWidth ?? 0;
    const next = clamp(cur + dx, 90, totalW - 90 - 5);
    scenesW = next;
    setScenesW(canvasUuid, next);
  }

  // ---- lifecycle -------------------------------------------------------------
  onMount(() => {
    defaultCanvas.start();
    canvasStore.start();
    void loadScenes();
    void loadLinks();
    // Live status (fetch + multistream.changed + outputBinding.changed) is owned by
    // the shared store; liveState derives off it. Keep the subscription for the
    // dock's lifetime.
    const offStatus = multistreamStatusStore.subscribe();

    reportRect();
    // Observe BOTH the stage and the whole dock body: the stage catches aspect/size
    // changes; the dock body catches resizes/relayouts that move the stage without
    // changing its own box (splitter drags elsewhere, dock re-tiling). Both feed the
    // rAF-coalesced scheduler so a resize burst collapses to one send per frame.
    const ro = new ResizeObserver(scheduleRect);
    if (previewEl) {
      ro.observe(previewEl);
    }
    if (dockBodyEl) {
      ro.observe(dockBodyEl);
    }
    window.addEventListener("resize", scheduleRect);
    window.addEventListener("scroll", scheduleRect, true);

    // This canvas's own scene/item events carry its uuid.
    const offScenes = obs.on(EV.scenesChanged, (p) => {
      if (p.canvas === canvasUuid) {
        void loadScenes();
        void loadLinks();
      }
    });
    const offLinks = obs.on(EV.sceneLinkChanged, () => void loadLinks());
    // Renaming a Default (main) scene emits scenes.changed{canvas:null}; reload so
    // the 🔗 tooltip + submenu checks reflect the new main-scene name immediately.
    const offDefaultScenes = obs.on(EV.scenesChanged, (p) => {
      if (p.canvas == null) void loadLinks();
    });
    const offItems = obs.on(EV.sceneItemsChanged, (p) => {
      if (p.canvas === canvasUuid && (!p.scene || p.scene === currentScene)) {
        void loadItems();
      }
    });
    const offSel = obs.on(EV.sceneItemSelected, (p) => {
      if (p.canvas === canvasUuid && (!p.scene || p.scene === currentScene)) {
        // The whole set, so a preview multi-select (Ctrl-click or a rubber band) lands
        // here as the set it is rather than as its focused item alone.
        activeSurface.claimSource(surfaceOwner, canvasUuid, selection);
        selection.adoptPreview(p, items);
        // Drilling into a group in the preview picks from its children, so show them.
        void expandEnteredGroup(items, p.enteredGroup, setCollapsed);
      }
    });

    // Right-click in this canvas's overlay: filter to our uuid in this window with
    // a real hit, then map the device-px cursor to viewport coords via the rect.
    const offMenu = obs.on(EV.previewContextMenu, (p) => {
      if (p.canvas !== canvasUuid || p.window !== WINDOW_ID || !previewEl) {
        return;
      }
      const { x, y } = mapOverlayCursor(previewEl, p);
      void (async () => {
        // Read the surface's view first: both menu shapes end with the Scale and
        // Lock Preview entries, which describe the preview itself rather than
        // whatever is (or is not) under the cursor.
        const view = await fetchPreviewView(canvasUuid);
        if (p.id == null) {
          menu = { x, y, items: [...buildEmptyItems(), ...previewViewMenuItems(view, canvasUuid, report)], suspendOverlay: true };
          return;
        }
        const deint = p.source
          ? await fetchDeint(p.source)
          : { mode: "disable" as const, fieldOrder: "top" as const };
        const transitionTypeList = await transitionTypes().catch(() => []);
        menu = {
          x,
          y,
          items: [...buildPreviewItems(p, deint, transitionTypeList), ...previewViewMenuItems(view, canvasUuid, report)],
          suspendOverlay: true,
        };
      })();
    });

    const releaseGuard = usePreviewPointerGuard();

    return () => {
      ro.disconnect();
      releaseGuard();
      if (rectRaf) {
        cancelAnimationFrame(rectRaf);
      }
      window.removeEventListener("resize", scheduleRect);
      window.removeEventListener("scroll", scheduleRect, true);
      offScenes();
      offLinks();
      offDefaultScenes();
      offItems();
      offSel();
      offStatus();
      offMenu();
      destroyPreview(canvasUuid);
    };
  });

  // Reload the source list whenever this canvas's current scene changes.
  $effect(() => {
    void currentScene;
    void loadItems();
  });

  // Keep the per-canvas selection fresh against the latest list; clears on scene change.
  $effect(() => {
    selection.reconcile(currentScene, items);
  });

  // Hand the surface back so the app-level shortcuts stop addressing a canvas that is no
  // longer mounted — only when the store still points at ours (another dock may have
  // claimed it since).
  onDestroy(() => {
    activeSurface.release(surfaceOwner);
  });

  // App-level F2 / Delete, addressed by canvas because scene-item ids are per-scene
  // counters and scene names are per-canvas, so a row from another surface's list can name
  // a different row in ours. All three actions land here: this dock owns its scene rows as
  // well as its source rows, and each branch calls the same function the dock's own
  // toolbar/context menu calls, so the keyboard inherits its error surface, its undo entry
  // and the bridge's last-scene refusal rather than re-deriving them. Each branch consumes
  // the request only once its row is confirmed present, so one nothing can serve is
  // reported to its sender rather than left to replay on a later mount.
  $effect(() => {
    const p = dockAction.pending;
    const action = p?.action;
    if (!p || p.canvas !== canvasUuid || !action) {
      return;
    }
    if (action.kind === "renameSource") {
      pendingRename.serve(action.ref, () => dockAction.consume(p.seq));
      return;
    }
    if (!scenes.some((sc) => sc.name === action.name)) {
      return;
    }
    if (!dockAction.consume(p.seq)) {
      return;
    }
    switch (action.kind) {
      case "renameScene":
        beginRenameScene(action.name);
        break;
      case "removeScene":
        removeScene(action.name);
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
      scenes.map((sc) => sc.name),
    );
  });

  // Hide our overlay while a modal suspends previews and re-assert on clear, handing
  // the stage to the still and back.
  const freeze = new PreviewFreeze();
  // Something is painting video over the stage: the native surface, or the still
  // standing in for it. Either way the DOM outline underneath must not show.
  const stageCovered = $derived(surfaceActive || freeze.frame !== null);
  $effect(() => syncPreviewGate(freeze, previewEl, surfaceDock));

  // Re-measure on any dock layout change. A reorder/move swaps panel positions
  // without resizing them, so ResizeObserver never fires and this canvas's overlay
  // would stay at its old screen position while the DOM slots swapped.
  $effect(() => {
    dockLayout.v;
    scheduleRect();
  });

  // The preview menu opens at the cursor inside the overlay (which sits above CEF
  // and would occlude it); suspend the overlay only for that menu, not row menus.
  $effect(() => {
    if (menu?.suspendOverlay) {
      return suspendPreview();
    }
  });
</script>

<div class="dock-body" bind:this={dockBodyEl}>
  <!-- Stage: aspect-correct surface the native overlay paints through; the chips
       are the mock's stage overlays (res top-left, LIVE top-right, scene label
       bottom-left). pointer-events:none so they never intercept overlay input. -->
  <div class="stage-area">
    <!-- onwheel forwards to the host's zoom; see the same handler in PreviewDock. -->
    <div
      class="stage"
      class:vertical
      class:active={stageCovered}
      bind:this={previewEl}
      onwheel={(e) => previewEl && forwardPreviewWheel(previewEl, e, canvasUuid)}
    >
      <!-- Stands in for the hidden native surface while an overlay is up. FIRST so the
           disabled-preview placeholder still stacks above it on DOM order alone. -->
      {#if freeze.frame}
        <img class="freeze" src={freeze.frame} alt="" aria-hidden="true" bind:this={freeze.img} />
      {/if}
      <!-- Occluded by the native surface whenever it paints, so the still has to
           occlude them too -- otherwise a right-click visibly redecorates the stage
           with chrome the live preview never shows. -->
      {#if !freeze.frame}
        {#if resText}
          <span class="res-chip">{resText}</span>
        {/if}
        {#if liveState === "live"}
          <span class="live-chip"><Icon name="dot" size={7} /> LIVE</span>
        {/if}
        <div class="scene-tag">
          <span class="scene-bar"></span>
          <span class="scene-pill">{currentScene ?? canvasName}</span>
        </div>
      {/if}
      {#if isPreviewDisabled(canvasUuid)}
        <div class="placeholder">
          <p class="ph-title">Preview disabled</p>
          <p class="ph-sub">Rendering is stopped to save GPU.</p>
          <Button variant="filled" face="label" onclick={enablePreview}>Re-enable Preview</Button>
        </div>
      {/if}
    </div>
  </div>

  {#if dockError.message}
    <p class="dock-msg err" role="alert">{dockError.message}</p>
  {/if}

  <!-- Drag the divider to trade preview height for the mini-lists' height. -->
  <Splitter orientation="column" onDrag={onEmbedDrag} />

  <!-- Embedded mini-lists: Scenes (left) + Sources (right), both resizable. -->
  <div class="embed" bind:this={embedEl} style:--embed-h={embedH != null ? embedH + "px" : null}>
    <div class="col scenes-col" bind:this={scenesColEl} style:--scenes-w={scenesW != null ? scenesW + "px" : null}>
      <div class="embed-head">Scenes</div>
      <ul class="list">
        {#each filteredScenes as scene, idx (scene.name)}
          <li
            class="es-row"
            class:on={scene.current}
            class:dropTarget={sceneDnd.isDropTarget(idx, scene.name)}
            draggable={!sceneFiltering}
            ondragstart={(e) => sceneDnd.onDragStart(e, scene.name)}
            ondragover={(e) => sceneDnd.onDragOver(e, idx)}
            ondrop={(e) => sceneDnd.onDrop(e, idx)}
            ondragend={sceneDnd.onDragEnd}
            oncontextmenu={(e) => openSceneMenu(e, scene.name)}
          >
            <span class="es-bar"></span>
            {#if renamingScene === scene.name}
              <input
                class="inline"
                bind:value={renameSceneTo}
                onkeydown={onRenameSceneKey}
                onblur={commitRenameScene}
                use:selectOnMount
              />
            {:else}
              <button
                class="es-label"
                ondblclick={() => beginRenameScene(scene.name)}
                onclick={() => setCurrentScene(scene.name)}>{scene.name}</button
              >
              {#if linksByCanvasScene.get(scene.name)?.length}
                <span
                  class="link-badge"
                  title={"Linked to: " + linksByCanvasScene.get(scene.name)!.join(", ")}
                >
                  <Icon name="link" size={11} />
                </span>
              {/if}
            {/if}
          </li>
        {/each}
        {#if addingScene}
          <li class="es-row">
            <span class="es-bar"></span>
            <input
              class="inline"
              placeholder="Scene name"
              bind:value={newSceneName}
              onkeydown={onAddSceneKey}
              onblur={commitAddScene}
              use:selectOnMount
            />
          </li>
        {/if}
        {#if loaded && filteredScenes.length === 0 && !addingScene}
          <li class="es-row empty">{sceneFiltering ? "No matches" : "No scenes"}</li>
        {/if}
      </ul>
      <ListToolbar left={scenesLeft} right={scenesRight}>
        {#snippet middle()}
          <FilterReveal bind:value={sceneFilter} />
        {/snippet}
      </ListToolbar>
    </div>

    <!-- Drag to trade Scenes width for Sources width. -->
    <Splitter orientation="row" onDrag={onScenesDrag} />

    <div class="col sources-col">
      <div class="embed-head">Sources{#if currentScene}<span class="embed-head-name dot-sep">{currentScene}</span>{/if}</div>
      {#if sourceRows.length > 0}
        <SourceTree
          rows={sourceRows}
          {selection}
          variant="embed"
          label="{canvasName} sources"
          filtering={sourceFiltering}
          {renaming}
          bind:renameTo
          onSelect={onTreeSelect}
          onRenameCommit={() => void commitRenameSource()}
          onRenameCancel={() => (renaming = null)}
          onToggleVisible={(item) => void toggleVisible(item)}
          onToggleLocked={(item) => void toggleLocked(item)}
          onOpenProperties={openProperties}
          onContextMenu={(e, item) => void openSourceMenu(e, item)}
          onReorder={(item, to) => void reorderTo(item, to)}
          onSetCollapsed={setCollapsed}
        />
      {:else}
        <ul class="list">
          {#if currentScene}
            <li class="es-row empty">{sourceFiltering ? "No matches" : "No sources"}</li>
          {/if}
        </ul>
      {/if}
      <ListToolbar left={sourcesLeft} right={sourcesRight}>
        {#snippet middle()}
          <FilterReveal bind:value={sourceFilter} />
        {/snippet}
      </ListToolbar>
    </div>
  </div>

</div>

{#if addingSource}
  <AddSourceModal
    canvas={canvasUuid}
    scene={currentScene}
    onCreated={onSourceCreated}
    onClose={() => (addingSource = false)}
  />
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
  /* This composite owns its inner scroll regions, so the body itself never
     scrolls (the shared .dock-body default sets overflow:auto). */
  .dock-body {
    min-height: 0;
    overflow: hidden;
    background: var(--color-base);
  }

  /* ---- stage ----------------------------------------------------------- */
  .stage-area {
    flex: 1;
    min-height: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 12px;
    background: var(--color-base);
  }
  /* The native overlay HWND paints this exact element; stays transparent so the
     video shows through. Aspect: 16:9 by default, 9:16 for a vertical canvas. */
  .freeze {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    object-fit: contain;
    background: var(--color-base);
  }
  .stage {
    position: relative;
    background: transparent;
    box-shadow: 0 0 0 1px var(--color-border);
    width: 100%;
    aspect-ratio: 16 / 9;
    max-width: 100%;
    max-height: 100%;
  }
  /* Whatever paints the stage — the native surface or the held still — covers the
     whole of it, so drop the DOM outline; otherwise it lingers as a ghost frame
     (visible drifting during a resize while the async native window trails). It
     returns as the placeholder frame only when the stage is genuinely empty. */
  .stage.active {
    box-shadow: none;
  }
  .stage.vertical {
    width: auto;
    height: 100%;
    aspect-ratio: 9 / 16;
    max-height: 100%;
  }
  .res-chip {
    position: absolute;
    left: 9px;
    top: 9px;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.08em;
    color: rgba(255, 255, 255, 0.55);
    border: var(--border-weight) solid rgba(255, 255, 255, 0.16);
    padding: 2px 6px;
    pointer-events: none;
  }
  .live-chip {
    position: absolute;
    right: 9px;
    top: 9px;
    display: flex;
    align-items: center;
    gap: 4px;
    font-family: var(--font-mono);
    font-size: 9px;
    color: #fff;
    background: var(--color-live);
    padding: 2px 6px;
    pointer-events: none;
  }
  .scene-tag {
    position: absolute;
    left: 0;
    bottom: 0;
    display: flex;
    align-items: stretch;
    pointer-events: none;
  }
  .scene-bar {
    width: 4px;
    background: var(--color-accent);
  }
  .scene-pill {
    background: rgba(8, 8, 10, 0.78);
    padding: 6px 11px;
    font-size: 11px;
    font-weight: 600;
    color: #fff;
    max-width: 100%;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .placeholder {
    position: absolute;
    inset: 0;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 4px;
    text-align: center;
    padding: 20px;
    pointer-events: none;
  }
  /* The disabled-preview placeholder adds a real action (Re-enable); opt back into
     pointer events for just that button rather than the whole overlay. */
  .placeholder :global(button) {
    pointer-events: auto;
    margin-top: 10px;
  }
  .ph-title {
    margin: 0;
    font-family: var(--font-ui);
    font-size: 12px;
    font-weight: 600;
    color: var(--color-dim);
  }
  .ph-sub {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 10.5px;
    color: var(--color-muted);
  }

  /* ---- embedded scenes / sources lists --------------------------------- */
  .embed {
    flex: 0 0 var(--embed-h, 154px);
    display: flex;
    min-height: 0;
  }
  .col {
    display: flex;
    flex-direction: column;
    min-height: 0;
    min-width: 0;
    background: var(--color-surface);
  }
  .scenes-col {
    flex: 0 0 var(--scenes-w, 42%);
  }
  .sources-col {
    flex: 1;
  }
  .embed-head {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    height: 23px;
    padding: 0 9px;
    background: var(--color-surface);
    border-bottom: var(--border-weight) solid var(--color-border-2);
    font-size: 10px;
    font-weight: 600;
    color: var(--color-dim);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  /* The scene name is user data, so it keeps whatever case the user typed. */
  .embed-head-name {
    text-transform: none;
    letter-spacing: normal;
  }
  .list {
    list-style: none;
    margin: 0;
    padding: 0;
    overflow: auto;
    flex: 1;
    min-height: 0;
  }

  .embed :global(.es-row) {
    position: relative;
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 5px 9px 5px 11px;
    cursor: pointer;
    border-bottom: var(--border-weight) solid var(--color-border-2);
  }
  .embed :global(.es-row.on) {
    background: color-mix(in srgb, var(--color-accent) 11%, transparent);
  }
  /* Drag-reorder drop indicator. Outline avoids layout shift and the inline
     box-shadow the color tag already uses. */
  .es-row.dropTarget {
    outline: var(--border-weight) solid var(--color-accent);
    outline-offset: -1px;
  }
  .es-bar {
    position: absolute;
    left: 0;
    top: 0;
    bottom: 0;
    width: 3px;
    background: transparent;
  }
  .es-row.on .es-bar {
    background: var(--color-accent);
  }
  .embed :global(.es-label) {
    flex: 1;
    min-width: 0;
    text-align: left;
    background: none;
    border: 0;
    padding: 0;
    cursor: pointer;
    font-family: var(--font-ui);
    font-size: 11px;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .embed :global(.es-row.on .es-label) {
    color: var(--color-accent);
    font-weight: 600;
  }
  /* Twin of the .dock-row focus ring in app.css — see the note there for why the indicator
     sits on the row, why it is --color-text rather than the accent, and why :where(). */
  .es-row:where(:has(.es-label:focus-visible)) {
    outline: 2px solid var(--color-text);
    outline-offset: -2px;
  }
  .es-row .es-label:focus-visible {
    outline: none;
  }
  .link-badge {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    color: var(--color-dim);
    cursor: default;
  }
  .es-row.empty {
    cursor: default;
    color: var(--color-muted);
    font-size: 10px;
    padding: 6px 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }

  .inline {
    flex: 1;
    min-width: 0;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-accent);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 2px 5px;
  }
  .inline:focus {
    outline: none;
  }

  /* This dock only ever shows an error message; keep it tight, no tracking. */
  .dock-msg {
    margin: 0;
    padding: 6px 9px;
    font-size: 10px;
  }
  .dock-msg.err {
    color: var(--color-live);
  }
</style>
