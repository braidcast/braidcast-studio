<script lang="ts">
  import { onMount } from "svelte";
  import {
    obs,
    type SceneInfo,
    type SceneItem,
    type ReorderDirection,
    type MultistreamState,
    type SceneLinkInfo,
  } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { previewSuspended, suspendPreview } from "$lib/stores/previewGate.svelte";
import { dockLayout } from "$lib/docking/dockLayoutSignal.svelte";
  import { WINDOW_ID } from "$lib/utils/windowContext";
  import { syncPreviewRect, hidePreview as hidePreviewSurface, destroyPreview, mapOverlayCursor } from "$lib/docking/previewSurface";
  import { isPreviewDisabled, setPreviewDisabled } from "$lib/docking/previewDisabledStore.svelte";
  import ContextMenu, { type ContextMenuItem } from "$lib/menus/ContextMenu.svelte";
  import { clipboard } from "$lib/stores/clipboardStore.svelte";
  import { openFilters } from "$lib/dialogs/filterDialogOpener.svelte";
  import { transformMenu } from "$lib/menus/transformMenu";
  import { scaleFilterMenu } from "$lib/menus/scaleFilterMenu";
  import { deinterlaceMenu } from "$lib/menus/deinterlaceMenu";
  import { colorMenu } from "$lib/menus/colorMenu";
  import type { DeinterlaceMode, DeinterlaceFieldOrder } from "$lib/api/bridge";
  import { defaultCanvas } from "$lib/docks/defaultCanvasStore.svelte";
  import { canvasStore } from "$lib/stores/canvasStore.svelte";
  import { callOrToast } from "$lib/utils/callToast";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import AddSourceModal from "$lib/dialogs/add-source/AddSourceModal.svelte";
  import PropertiesModal from "$lib/properties/PropertiesModal.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import ListToolbar, { type ToolAction } from "$lib/docking/ListToolbar.svelte";
  import FilterReveal from "$lib/docking/FilterReveal.svelte";
  import Splitter from "$lib/docking/Splitter.svelte";
  import { getPaneSizes, setEmbedH, setScenesW } from "$lib/docking/canvasPaneSizes";
  import { multistreamStatusStore } from "$lib/stores/multistreamStatusStore.svelte";

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

  let error = $state<string | null>(null);
  function report(e: unknown) {
    error = (e as Error).message;
  }

  function focusOnMount(node: HTMLInputElement) {
    node.focus();
    node.select();
  }

  // One context menu for the whole dock (scene rows, source rows, and the preview
  // all use it). `suspendOverlay` is set only by the preview menu: it opens over
  // the native overlay and must blank it; row menus open in the list area and must
  // not (blanking would flash the preview off for no reason).
  let menu = $state<{ x: number; y: number; items: (ContextMenuItem | null)[]; suspendOverlay?: boolean } | null>(null);

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

  function reportRect() {
    if (!previewEl) {
      return;
    }
    // While a modal/overlay holds the preview gate, or the user disabled this
    // canvas's preview, never re-assert the rect: a stray resize/scroll/
    // ResizeObserver tick would otherwise raise this canvas's native child window
    // back above CEF, over the modal or over the placeholder. While disabled the
    // surface is already destroyed (see disablePreview below), so hidePreview here
    // is a harmless idempotent no-op.
    if (previewSuspended() || isPreviewDisabled(canvasUuid)) {
      hidePreview();
      return;
    }
    // syncPreviewRect asserts the rect (or hides on a zero/hidden box) and reports
    // whether the surface is now painting; mirror that into surfaceActive so the DOM
    // stage outline drops only while the native surface owns the stage.
    surfaceActive = syncPreviewRect(previewEl, canvasUuid);
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

  async function loadScenes() {
    try {
      const list = await obs.call("scenes.list", { canvas: canvasUuid });
      scenes = list;
      currentScene = list.find((s) => s.current)?.name ?? null;
      error = null;
    } catch (e) {
      report(e);
    } finally {
      loaded = true;
    }
  }
  function setCurrentScene(name: string) {
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
  let sceneFilter = $state("");
  let filteredScenes = $derived(
    sceneFilter.trim()
      ? scenes.filter((s) => s.name.toLowerCase().includes(sceneFilter.trim().toLowerCase()))
      : scenes,
  );

  // Duplicates a scene from THIS canvas onto another canvas (a deep copy, unlike
  // the same-canvas sceneItems.duplicate above which is a ref duplicate). See
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
      const as = r.name !== sceneName ? ` as "${r.name}"` : "";
      showToast(`Duplicated "${sceneName}"${to}${as}`, r.name);
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
    menu = {
      x: e.clientX,
      y: e.clientY,
      items: [
        { label: "Rename", action: () => beginRenameScene(name) },
        { label: "Link to", children: linkChildren },
        { label: "Duplicate to canvas", children: duplicateChildren },
        null,
        { label: "Remove", danger: true, disabled: scenes.length <= 1, action: () => removeScene(name) },
      ],
    };
  }

  // ---- sources (current scene of this canvas) --------------------------------
  let items = $state<SceneItem[]>([]);
  let selectedId = $state<number | null>(null);

  // The Sources toolbar acts on the selected row (the OBS list convention); these
  // derive the target + its index so delete/move can disable when there is none.
  let selectedItem = $derived(items.find((i) => i.id === selectedId) ?? null);
  let selectedIdx = $derived(selectedId === null ? -1 : items.findIndex((i) => i.id === selectedId));

  // ---- source name filter (behind the toolbar reveal) ------------------------
  let sourceFilter = $state("");
  let sourceFiltering = $derived(sourceFilter.trim().length > 0);
  let filteredItems = $derived(
    sourceFiltering
      ? items.filter((i) => (i.source ?? "").toLowerCase().includes(sourceFilter.trim().toLowerCase()))
      : items,
  );

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
      if (selectedId !== null && !items.some((i) => i.id === selectedId)) {
        selectedId = null;
      }
    } catch (e) {
      report(e);
    }
  }
  function selectItem(item: SceneItem) {
    selectedId = item.id;
    void obs.call("preview.select", { canvas: canvasUuid, window: WINDOW_ID, scene: currentScene, id: item.id }).catch(() => {});
  }
  async function toggleVisible(item: SceneItem) {
    try {
      await obs.call("sceneItems.setVisible", {
        canvas: canvasUuid,
        scene: currentScene,
        id: item.id,
        visible: !item.visible,
      });
    } catch (e) {
      report(e);
    }
  }
  async function toggleLocked(item: SceneItem) {
    try {
      await obs.call("sceneItems.setLocked", {
        canvas: canvasUuid,
        scene: currentScene,
        id: item.id,
        locked: !item.locked,
      });
    } catch (e) {
      report(e);
    }
  }
  async function reorder(item: SceneItem, direction: ReorderDirection) {
    try {
      await obs.call("sceneItems.reorder", { canvas: canvasUuid, scene: currentScene, id: item.id, direction });
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
    if (sourceFiltering) {
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
      await obs.call("sceneItems.reorder", { canvas: canvasUuid, scene: currentScene, id, to });
    } catch (e) {
      report(e);
    }
  }
  async function remove(item: SceneItem) {
    try {
      await obs.call("sceneItems.remove", { canvas: canvasUuid, scene: currentScene, id: item.id });
    } catch (e) {
      report(e);
    }
  }

  // ---- add source / properties (scoped to this canvas's current scene) -------
  let addingSource = $state(false);
  let propsForSource = $state<string | null>(null);
  function onSourceCreated(created: { id: number; source: string }) {
    addingSource = false;
    selectedId = created.id;
    propsForSource = created.source;
    void loadItems();
  }
  function openProperties(item: SceneItem) {
    if (item.source) {
      propsForSource = item.source;
    }
  }

  // ---- source rename (scoped to this canvas's current scene) -----------------
  let renamingId = $state<number | null>(null);
  let renameTo = $state("");

  function beginRenameSource(item: SceneItem) {
    renamingId = item.id;
    renameTo = item.source ?? "";
  }
  async function commitRenameSource() {
    const id = renamingId;
    const name = renameTo.trim();
    renamingId = null;
    if (id === null || !name) {
      return;
    }
    try {
      await obs.call("sources.rename", { canvas: canvasUuid, scene: currentScene, id, name });
    } catch (e) {
      report(e);
    }
  }
  function onRenameSourceKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      void commitRenameSource();
    } else if (e.key === "Escape") {
      renamingId = null;
    }
  }

  // ---- clipboard actions (copy/paste/duplicate/filters/transform/group) ------
  // Every call carries this canvas's uuid (the additional-canvas path); paste
  // lands in this canvas's current scene. Filter copy/paste keys off the source
  // name and so shares the same clipboard slots as the global SourcesDock.
  function copySource(item: SceneItem) {
    if (item.source) {
      clipboard.source = { ref: item.source, name: item.source };
    }
  }
  async function pasteSource() {
    if (!clipboard.source || !currentScene) {
      return;
    }
    try {
      await obs.call("sources.addExisting", { canvas: canvasUuid, scene: currentScene, name: clipboard.source.ref });
    } catch (e) {
      report(e);
    }
  }
  async function duplicateItem(item: SceneItem) {
    try {
      await obs.call("sources.duplicate", { canvas: canvasUuid, scene: currentScene, id: item.id });
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
      clipboard.transform = await obs.call("sceneItems.getTransform", {
        canvas: canvasUuid,
        scene: currentScene,
        id: item.id,
      });
    } catch (e) {
      report(e);
    }
  }
  async function pasteTransform(item: SceneItem) {
    if (!clipboard.transform) {
      return;
    }
    try {
      await obs.call("sceneItems.setTransform", {
        canvas: canvasUuid,
        scene: currentScene,
        id: item.id,
        transform: clipboard.transform,
      });
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
      await obs.call("sceneItems.ungroup", { canvas: canvasUuid, scene: currentScene, id: item.id });
    } catch (e) {
      report(e);
    }
  }

  // Deinterlacing is a source-level property (no canvas), fetched just-in-time when
  // the menu opens. Falls back to disabled state on error.
  async function fetchDeint(source: string): Promise<{ mode: DeinterlaceMode; fieldOrder: DeinterlaceFieldOrder }> {
    try {
      return await obs.call("sources.getDeinterlace", { source });
    } catch {
      return { mode: "disable", fieldOrder: "top" };
    }
  }

  async function openSourceMenu(e: MouseEvent, item: SceneItem) {
    e.preventDefault();
    const x = e.clientX;
    const y = e.clientY;
    const idx = items.findIndex((i) => i.id === item.id);
    const deint = item.source ? await fetchDeint(item.source) : { mode: "disable" as const, fieldOrder: "top" as const };
    menu = {
      x,
      y,
      items: [
        { label: "Filters", disabled: !item.source, action: () => item.source && openFilters(item.source) },
        ...(item.interactive && item.source
          ? [{ label: "Interact", action: () => void obs.call("sources.interact", { source: item.source }).catch(report) }]
          : []),
        transformMenu({ canvas: canvasUuid, scene: currentScene ?? undefined, id: item.id }, item.source ?? "(unnamed)"),
        { label: "Rename", action: () => beginRenameSource(item) },
        scaleFilterMenu(item.scaleFilter, (filter) =>
          void obs
            .call("sceneItems.setScaleFilter", { canvas: canvasUuid, scene: currentScene, id: item.id, filter })
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
          void obs
            .call("sceneItems.setColor", { canvas: canvasUuid, scene: currentScene, id: item.id, color })
            .catch(report),
        ),
        {
          label: "Screenshot",
          disabled: !item.source,
          action: () =>
            void obs
              .call("screenshot.takeSource", { canvas: canvasUuid, scene: currentScene, id: item.id })
              .catch(report),
        },
        null,
        { label: "Copy", disabled: !item.source, action: () => copySource(item) },
        { label: "Paste", disabled: !clipboard.source, action: () => void pasteSource() },
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
        { label: "Move Up", disabled: idx === 0, action: () => void reorder(item, "up") },
        { label: "Move Down", disabled: idx === items.length - 1, action: () => void reorder(item, "down") },
        { label: "Move to Top", disabled: idx === 0, action: () => void reorder(item, "top") },
        { label: "Move to Bottom", disabled: idx === items.length - 1, action: () => void reorder(item, "bottom") },
        // Projector entries hidden pending the projector redesign.
        null,
        { label: "Remove", danger: true, action: () => void remove(item) },
      ],
    };
  }

  // ---- preview right-click menu (this canvas's hit scene item) ---------------
  // No Properties (matches the row menu, which omits it for additional-canvas
  // private sources). Every call carries this canvas's uuid + scene + item id.
  function buildPreviewItems(
    p: {
      scene: string | null;
      id: number | null;
      source: string | null;
      visible: boolean;
      locked: boolean;
    },
    deint: { mode: DeinterlaceMode; fieldOrder: DeinterlaceFieldOrder },
  ): (ContextMenuItem | null)[] {
    const call = (method: string, params: Record<string, unknown>) =>
      obs.call(method, { canvas: canvasUuid, scene: p.scene, id: p.id, ...params }).catch(report);
    const currentFilter = items.find((i) => i.id === p.id)?.scaleFilter ?? "disable";
    const currentColor = items.find((i) => i.id === p.id)?.color ?? "";
    return [
      ...(p.id != null
        ? [transformMenu({ canvas: canvasUuid, scene: p.scene ?? undefined, id: p.id }, p.source ?? "(unnamed)")]
        : []),
      scaleFilterMenu(currentFilter, (filter) => void call("sceneItems.setScaleFilter", { filter })),
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
      disabled: !currentScene || scenes.length <= 1,
      onClick: () => currentScene && removeScene(currentScene),
    },
  ]);
  let sourcesLeft = $derived<ToolAction[]>([
    { icon: "plus", title: "Add source", disabled: !currentScene, onClick: () => (addingSource = true) },
    { icon: "trash", title: "Delete source", disabled: !selectedItem, onClick: () => selectedItem && void remove(selectedItem) },
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
      disabled: sourceFiltering || selectedIdx <= 0,
      onClick: () => selectedItem && void reorder(selectedItem, "up"),
    },
    {
      icon: "down",
      title: "Move down",
      disabled: sourceFiltering || selectedIdx < 0 || selectedIdx >= items.length - 1,
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

  function clamp(v: number, lo: number, hi: number): number {
    return Math.min(Math.max(v, lo), Math.max(lo, hi));
  }
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
        selectedId = p.id;
      }
    });

    // Right-click in this canvas's overlay: filter to our uuid in this window with
    // a real hit, then map the device-px cursor to viewport coords via the rect.
    const offMenu = obs.on(EV.previewContextMenu, (p) => {
      if (p.canvas !== canvasUuid || p.window !== WINDOW_ID || p.id == null || !previewEl) {
        return;
      }
      const { x, y } = mapOverlayCursor(previewEl, p);
      void (async () => {
        const deint = p.source
          ? await fetchDeint(p.source)
          : { mode: "disable" as const, fieldOrder: "top" as const };
        menu = { x, y, items: buildPreviewItems(p, deint), suspendOverlay: true };
      })();
    });

    return () => {
      ro.disconnect();
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

  // Hide our overlay while a modal suspends previews; re-assert on clear.
  $effect(() => {
    if (previewSuspended()) {
      hidePreview();
    } else {
      reportRect();
    }
  });

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
    <div class="stage" class:vertical class:active={surfaceActive} bind:this={previewEl}>
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
      {#if isPreviewDisabled(canvasUuid)}
        <div class="placeholder">
          <p class="ph-title">Preview disabled</p>
          <p class="ph-sub">Rendering is stopped to save GPU.</p>
          <button class="accent" onclick={enablePreview}>Re-enable Preview</button>
        </div>
      {/if}
    </div>
  </div>

  {#if error}
    <p class="dock-msg err">{error}</p>
  {/if}

  <!-- Drag the divider to trade preview height for the mini-lists' height. -->
  <Splitter orientation="column" onDrag={onEmbedDrag} />

  <!-- Embedded mini-lists: Scenes (left) + Sources (right), both resizable. -->
  <div class="embed" bind:this={embedEl} style:--embed-h={embedH != null ? embedH + "px" : null}>
    <div class="col scenes-col" bind:this={scenesColEl} style:--scenes-w={scenesW != null ? scenesW + "px" : null}>
      <div class="embed-head">Scenes</div>
      <ul class="list">
        {#each filteredScenes as scene (scene.name)}
          <li class="es-row" class:on={scene.current} oncontextmenu={(e) => openSceneMenu(e, scene.name)}>
            <span class="es-bar"></span>
            {#if renamingScene === scene.name}
              <input
                class="inline"
                bind:value={renameSceneTo}
                onkeydown={onRenameSceneKey}
                onblur={commitRenameScene}
                use:focusOnMount
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
              use:focusOnMount
            />
          </li>
        {/if}
        {#if loaded && filteredScenes.length === 0 && !addingScene}
          <li class="es-row empty">{sceneFilter.trim() ? "No matches" : "No scenes"}</li>
        {/if}
      </ul>
      <ListToolbar left={scenesLeft}>
        {#snippet middle()}
          <FilterReveal bind:value={sceneFilter} />
        {/snippet}
      </ListToolbar>
    </div>

    <!-- Drag to trade Scenes width for Sources width. -->
    <Splitter orientation="row" onDrag={onScenesDrag} />

    <div class="col sources-col">
      <div class="embed-head">Sources{currentScene ? " · " + currentScene : ""}</div>
      <ul class="list">
        {#each filteredItems as item, idx (item.id)}
          <li
            class="es-row src"
            class:on={item.id === selectedId}
            class:hidden-src={!item.visible}
            class:dropTarget={dragOverIdx === idx && dragId !== null && dragId !== item.id}
            style:box-shadow={item.color ? `inset 3px 0 0 ${item.color}` : null}
            draggable={!sourceFiltering}
            ondragstart={(e) => onDragStart(e, item)}
            ondragover={(e) => onDragOver(e, idx)}
            ondrop={(e) => onDrop(e, idx)}
            ondragend={onDragEnd}
            oncontextmenu={(e) => void openSourceMenu(e, item)}
          >
            <button
              class="es-eye"
              class:off={!item.visible}
              title={item.visible ? "Hide" : "Show"}
              aria-label={item.visible ? "Hide" : "Show"}
              onclick={() => void toggleVisible(item)}><Icon name={item.visible ? "eye" : "eye-off"} size={14} /></button
            >
            {#if renamingId === item.id}
              <input
                class="inline"
                bind:value={renameTo}
                onkeydown={onRenameSourceKey}
                onblur={commitRenameSource}
                use:focusOnMount
              />
            {:else}
              <button class="es-label" onclick={() => selectItem(item)}>{item.source ?? "(unnamed)"}</button>
            {/if}
            <button
              class="es-lock"
              class:locked={item.locked}
              title={item.locked ? "Unlock" : "Lock"}
              aria-label={item.locked ? "Unlock" : "Lock"}
              onclick={() => void toggleLocked(item)}><Icon name={item.locked ? "lock" : "lock-open"} size={12} /></button
            >
          </li>
        {/each}
        {#if currentScene && filteredItems.length === 0}
          <li class="es-row empty">{sourceFiltering ? "No matches" : "No sources"}</li>
        {/if}
      </ul>
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
  .stage {
    position: relative;
    background: transparent;
    box-shadow: 0 0 0 1px var(--color-border);
    width: 100%;
    aspect-ratio: 16 / 9;
    max-width: 100%;
    max-height: 100%;
  }
  /* Once the native surface is asserted it paints the whole stage, so drop the DOM
     outline — otherwise it lingers as a ghost frame (visible drifting during a
     resize while the async native window trails). It returns as the placeholder
     frame whenever the surface is hidden/suspended. */
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
  .placeholder button {
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
    letter-spacing: 0.02em;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .list {
    list-style: none;
    margin: 0;
    padding: 0;
    overflow: auto;
    flex: 1;
    min-height: 0;
  }

  .es-row {
    position: relative;
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 5px 9px 5px 11px;
    cursor: pointer;
    border-bottom: var(--border-weight) solid var(--color-border-2);
  }
  .es-row.src {
    padding: 5px 10px;
    cursor: default;
  }
  .es-row.on {
    background: color-mix(in srgb, var(--color-accent) 11%, transparent);
  }
  /* Drag-reorder drop indicator. Outline avoids layout shift and the inline
     box-shadow the color tag already uses. */
  .es-row.src.dropTarget {
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
  .es-label {
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
  .es-row.on .es-label {
    color: var(--color-accent);
    font-weight: 600;
  }
  .es-row.hidden-src .es-label {
    color: var(--color-muted);
    text-decoration: line-through;
  }
  .link-badge {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    color: var(--color-dim);
    cursor: default;
  }
  .es-eye {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    justify-content: center;
    width: 17px;
    height: 17px;
    background: none;
    border: 0;
    padding: 0;
    cursor: pointer;
    color: var(--color-dim);
  }
  .es-eye.off {
    color: var(--color-muted);
  }
  .es-lock {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    justify-content: center;
    width: 17px;
    height: 17px;
    background: none;
    border: 0;
    padding: 0;
    cursor: pointer;
    color: var(--color-muted);
  }
  .es-lock.locked {
    color: var(--color-dim);
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
