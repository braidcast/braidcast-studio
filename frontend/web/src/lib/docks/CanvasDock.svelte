<script lang="ts">
  import { onMount } from "svelte";
  import {
    obs,
    type SceneInfo,
    type SceneItem,
    type ReorderDirection,
    type MultistreamState,
    type CanvasInfo,
    type SceneLinkInfo,
  } from "../bridge";
  import { openSettings } from "../settingsOpener.svelte";
  import { previewSuspended, suspendPreview } from "../previewGate.svelte";
  import { WINDOW_ID } from "../windowContext";
  import ContextMenu, { type ContextMenuItem } from "../ContextMenu.svelte";
  import { clipboard } from "../clipboardStore.svelte";
  import { openFilters } from "../filterDialogOpener.svelte";
  import { openTransform } from "../transformOpener.svelte";
  import { prefetchMonitors, projectorItems } from "../projectorMenu";
  import { scaleFilterMenu } from "../scaleFilterMenu";
  import { deinterlaceMenu } from "../deinterlaceMenu";
  import { colorMenu } from "../colorMenu";
  import type { DeinterlaceMode, DeinterlaceFieldOrder } from "../bridge";
  import { defaultCanvas } from "./defaultCanvasStore.svelte";

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
  function noop() {}

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

  function reportRect() {
    if (!previewEl) {
      return;
    }
    const r = previewEl.getBoundingClientRect();
    // A native overlay HWND sits above CEF and ignores Dockview's tab visibility:
    // when this dock is tab-stacked in the background (display:none) or collapsed,
    // its overlay would keep painting at its last rect and bleed over whatever is
    // now on top. Detect the hidden/zero case and hide the overlay instead.
    if (!previewEl.offsetParent || r.width < 1 || r.height < 1) {
      hidePreview();
      return;
    }
    obs
      .call("preview.setRect", {
        canvas: canvasUuid,
        window: WINDOW_ID,
        x: r.left,
        y: r.top,
        w: r.width,
        h: r.height,
        dpr: window.devicePixelRatio || 1,
      })
      .catch((e) => console.log("preview.setRect failed: " + (e as Error).message));
  }
  function hidePreview() {
    obs.call("preview.hide", { canvas: canvasUuid, window: WINDOW_ID }).catch(() => {});
  }
  function destroyPreview() {
    obs.call("preview.destroy", { canvas: canvasUuid, window: WINDOW_ID }).catch(() => {});
  }

  // ---- canvas geometry (stage res chip + aspect) -----------------------------
  // Display-only read: the mock stage needs the canvas resolution (res chip) and
  // aspect (9:16 vertical vs 16:9). CanvasDock receives only uuid+name, so look
  // this canvas up in canvas.list and refresh on canvas.changed. Nothing in the
  // scene/source/preview path depends on it.
  let canvasInfo = $state<CanvasInfo | null>(null);
  function loadCanvasInfo() {
    obs
      .call("canvas.list")
      .then((list) => (canvasInfo = list.find((c) => c.uuid === canvasUuid) ?? null))
      .catch(() => {});
  }
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
    menu = {
      x: e.clientX,
      y: e.clientY,
      items: [
        { label: "Rename", action: () => beginRenameScene(name) },
        { label: "Link to", children: linkChildren },
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

  async function loadItems() {
    if (!currentScene) {
      items = [];
      return;
    }
    try {
      items = await obs.call("sceneItems.list", { canvas: canvasUuid, scene: currentScene });
      if (selectedId !== null && !items.some((i) => i.id === selectedId)) {
        selectedId = null;
      }
    } catch (e) {
      report(e);
    }
  }
  function selectItem(item: SceneItem) {
    selectedId = item.id;
    void obs.call("preview.select", { canvas: canvasUuid, window: WINDOW_ID, scene: currentScene, id: item.id });
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
  async function remove(item: SceneItem) {
    try {
      await obs.call("sceneItems.remove", { canvas: canvasUuid, scene: currentScene, id: item.id });
    } catch (e) {
      report(e);
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

  async function openSourceMenu(e: MouseEvent, item: SceneItem, idx: number) {
    e.preventDefault();
    const x = e.clientX;
    const y = e.clientY;
    const deint = item.source ? await fetchDeint(item.source) : { mode: "disable" as const, fieldOrder: "top" as const };
    menu = {
      x,
      y,
      items: [
        { label: "Filters", disabled: !item.source, action: () => item.source && openFilters(item.source) },
        ...(item.interactive && item.source
          ? [{ label: "Interact", action: () => void obs.call("sources.interact", { source: item.source }).catch(report) }]
          : []),
        {
          label: "Edit Transform",
          action: () =>
            openTransform(
              { canvas: canvasUuid, scene: currentScene ?? undefined, id: item.id },
              item.source ?? "(unnamed)",
            ),
        },
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
        ...(item.source ? [null, ...projectorItems({ kind: "source", name: item.source })] : []),
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
      {
        label: "Edit Transform",
        action: () =>
          p.id != null &&
          openTransform({ canvas: canvasUuid, scene: p.scene ?? undefined, id: p.id }, p.source ?? "(unnamed)"),
      },
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
      null,
      { label: p.visible ? "Hide" : "Show", action: () => void call("sceneItems.setVisible", { visible: !p.visible }) },
      { label: p.locked ? "Unlock" : "Lock", action: () => void call("sceneItems.setLocked", { locked: !p.locked }) },
      null,
      { label: "Move Up", action: () => void call("sceneItems.reorder", { direction: "up" }) },
      { label: "Move Down", action: () => void call("sceneItems.reorder", { direction: "down" }) },
      { label: "Move to Top", action: () => void call("sceneItems.reorder", { direction: "top" }) },
      { label: "Move to Bottom", action: () => void call("sceneItems.reorder", { direction: "bottom" }) },
      null,
      // Project this canvas (addressed by its uuid).
      ...projectorItems({ kind: "canvas", canvas: canvasUuid }),
      null,
      { label: "Remove", danger: true, action: () => void call("sceneItems.remove", {}) },
    ];
  }

  // ---- footer: polled live-status dot for this canvas ------------------------
  let liveState = $state<MultistreamState | "off">("off");
  function recomputeState(rows: { canvasUuid: string; state: MultistreamState }[]) {
    const mine = rows.filter((r) => r.canvasUuid === canvasUuid);
    if (mine.length === 0) {
      liveState = "off";
    } else if (mine.some((r) => r.state === "live")) {
      liveState = "live";
    } else if (mine.some((r) => r.state === "error")) {
      liveState = "error";
    } else if (mine.some((r) => r.state === "connecting")) {
      liveState = "connecting";
    } else {
      liveState = "idle";
    }
  }
  const STATE_COLOR: Record<MultistreamState | "off", string> = {
    off: "var(--color-muted)",
    idle: "var(--color-muted)",
    connecting: "var(--meter-yellow)",
    live: "var(--meter-green)",
    error: "var(--color-live)",
  };

  // ---- lifecycle -------------------------------------------------------------
  onMount(() => {
    prefetchMonitors();
    defaultCanvas.start();
    loadCanvasInfo();
    void loadScenes();
    void loadLinks();
    void (async () => {
      try {
        const res = await obs.call("multistream.status");
        recomputeState(res.outputs);
      } catch {
        // status optional; dot stays off
      }
    })();

    reportRect();
    const ro = new ResizeObserver(reportRect);
    if (previewEl) {
      ro.observe(previewEl);
    }
    window.addEventListener("resize", reportRect);
    window.addEventListener("scroll", reportRect, true);

    // This canvas's own scene/item events carry its uuid.
    const offScenes = obs.on("scenes.changed", (p) => {
      if (p.canvas === canvasUuid) {
        void loadScenes();
        void loadLinks();
      }
    });
    const offLinks = obs.on("sceneLink.changed", () => void loadLinks());
    // Renaming a Default (main) scene emits scenes.changed{canvas:null}; reload so
    // the 🔗 tooltip + submenu checks reflect the new main-scene name immediately.
    const offDefaultScenes = obs.on("scenes.changed", (p) => {
      if (p.canvas == null) void loadLinks();
    });
    const offItems = obs.on("sceneItems.changed", (p) => {
      if (p.canvas === canvasUuid && (!p.scene || p.scene === currentScene)) {
        void loadItems();
      }
    });
    const offSel = obs.on("sceneItem.selected", (p) => {
      if (p.canvas === canvasUuid && (!p.scene || p.scene === currentScene)) {
        selectedId = p.id;
      }
    });
    const offStatus = obs.on("multistream.changed", (p) => recomputeState(p.outputs));
    const offCanvasInfo = obs.on("canvas.changed", loadCanvasInfo);

    // Right-click in this canvas's overlay: filter to our uuid in this window with
    // a real hit, then map the device-px cursor to viewport coords via the rect.
    const offMenu = obs.on("preview.contextMenu", (p) => {
      if (p.canvas !== canvasUuid || p.window !== WINDOW_ID || p.id == null || !previewEl) {
        return;
      }
      const r = previewEl.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      const x = r.left + p.x / dpr;
      const y = r.top + p.y / dpr;
      void (async () => {
        const deint = p.source
          ? await fetchDeint(p.source)
          : { mode: "disable" as const, fieldOrder: "top" as const };
        menu = { x, y, items: buildPreviewItems(p, deint), suspendOverlay: true };
      })();
    });

    return () => {
      ro.disconnect();
      window.removeEventListener("resize", reportRect);
      window.removeEventListener("scroll", reportRect, true);
      offScenes();
      offLinks();
      offDefaultScenes();
      offItems();
      offSel();
      offStatus();
      offCanvasInfo();
      offMenu();
      destroyPreview();
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

  // The preview menu opens at the cursor inside the overlay (which sits above CEF
  // and would occlude it); suspend the overlay only for that menu, not row menus.
  $effect(() => {
    if (menu?.suspendOverlay) {
      return suspendPreview();
    }
  });
</script>

{#snippet icoPlus()}
  <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"
    ><line x1="12" y1="5" x2="12" y2="19" /><line x1="5" y1="12" x2="19" y2="12" /></svg
  >
{/snippet}
{#snippet icoTrash()}
  <svg
    width="13"
    height="13"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="1.7"
    stroke-linecap="round"
    stroke-linejoin="round"><path d="M4 7h16M9 7V5a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2M6 7l1 13h10l1-13" /></svg
  >
{/snippet}
{#snippet icoGear()}
  <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"
    ><circle cx="12" cy="12" r="3.1" /><path
      d="M12 2.5v3M12 18.5v3M2.5 12h3M18.5 12h3M5.1 5.1l2.1 2.1M16.8 16.8l2.1 2.1M18.9 5.1 16.8 7.2M7.2 16.8 5.1 18.9"
    /></svg
  >
{/snippet}
{#snippet icoUp()}
  <svg
    width="13"
    height="13"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="2"
    stroke-linecap="round"
    stroke-linejoin="round"><path d="M6 14l6-6 6 6" /></svg
  >
{/snippet}
{#snippet icoDown()}
  <svg
    width="13"
    height="13"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="2"
    stroke-linecap="round"
    stroke-linejoin="round"><path d="M6 10l6 6 6-6" /></svg
  >
{/snippet}
{#snippet icoEye(open: boolean)}
  {#if open}
    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
      ><path d="M2 12s3.6-6.5 10-6.5S22 12 22 12s-3.6 6.5-10 6.5S2 12 2 12Z" /><circle cx="12" cy="12" r="2.4" /></svg
    >
  {:else}
    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
      ><path d="M4 4 L20 20" /><path
        d="M9.5 5.7A10 10 0 0 1 12 5.5c6.4 0 10 6.5 10 6.5a17 17 0 0 1-2.7 3.4M6.2 7.6A17 17 0 0 0 2 12s3.6 6.5 10 6.5a10 10 0 0 0 3-.45"
      /></svg
    >
  {/if}
{/snippet}
{#snippet icoLock(locked: boolean)}
  {#if locked}
    <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"
      ><rect x="5" y="11" width="14" height="9" /><path d="M8 11V8a4 4 0 0 1 8 0v3" /></svg
    >
  {:else}
    <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"
      ><rect x="5" y="11" width="14" height="9" /><path d="M8 11V7a4 4 0 0 1 7.6-1.7" /></svg
    >
  {/if}
{/snippet}

<div class="dock-body">
  <!-- Stage: aspect-correct surface the native overlay paints through; the chips
       are the mock's stage overlays (res top-left, LIVE top-right, scene label
       bottom-left). pointer-events:none so they never intercept overlay input. -->
  <div class="stage-area">
    <div class="stage" class:vertical bind:this={previewEl}>
      {#if resText}
        <span class="res-chip">{resText}</span>
      {/if}
      {#if liveState === "live"}
        <span class="live-chip">● LIVE</span>
      {/if}
      <div class="scene-tag">
        <span class="scene-bar"></span>
        <span class="scene-pill">{currentScene ?? canvasName}</span>
      </div>
    </div>
  </div>

  {#if error}
    <p class="dock-msg err">{error}</p>
  {/if}

  <!-- Embedded mini-lists: Scenes (left, 42%) + Sources (right). -->
  <div class="embed">
    <div class="col scenes-col">
      <div class="embed-head">Scenes</div>
      <ul class="list">
        {#each scenes as scene (scene.name)}
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
                  title={"Linked to: " + linksByCanvasScene.get(scene.name)!.join(", ")}>🔗</span
                >
              {/if}
            {/if}
          </li>
        {/each}
        {#if loaded && scenes.length === 0}
          <li class="es-row empty">No scenes</li>
        {/if}
      </ul>
      <div class="toolbar">
        <button class="tool-btn" title="Add scene (coming soon)" disabled onclick={noop}>{@render icoPlus()}</button>
        <button
          class="tool-btn"
          title="Delete scene"
          disabled={!currentScene || scenes.length <= 1}
          onclick={() => currentScene && removeScene(currentScene)}>{@render icoTrash()}</button
        >
        <button class="tool-btn" title="Scene settings (coming soon)" disabled onclick={noop}>{@render icoGear()}</button>
      </div>
    </div>

    <div class="col sources-col">
      <div class="embed-head">Sources{currentScene ? " · " + currentScene : ""}</div>
      <ul class="list">
        {#each items as item, idx (item.id)}
          <li
            class="es-row src"
            class:on={item.id === selectedId}
            class:hidden-src={!item.visible}
            style:box-shadow={item.color ? `inset 3px 0 0 ${item.color}` : null}
            oncontextmenu={(e) => void openSourceMenu(e, item, idx)}
          >
            <button
              class="es-eye"
              class:off={!item.visible}
              title={item.visible ? "Hide" : "Show"}
              onclick={() => void toggleVisible(item)}>{@render icoEye(item.visible)}</button
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
              onclick={() => void toggleLocked(item)}>{@render icoLock(item.locked)}</button
            >
          </li>
        {/each}
        {#if currentScene && items.length === 0}
          <li class="es-row empty">No sources</li>
        {/if}
      </ul>
      <div class="toolbar">
        <button class="tool-btn" title="Add source (coming soon)" disabled onclick={noop}>{@render icoPlus()}</button>
        <button
          class="tool-btn"
          title="Delete source"
          disabled={!selectedItem}
          onclick={() => selectedItem && void remove(selectedItem)}>{@render icoTrash()}</button
        >
        <button class="tool-btn" title="Properties (coming soon)" disabled onclick={noop}>{@render icoGear()}</button>
        <div class="tool-spacer"></div>
        <button
          class="tool-btn"
          title="Move up"
          disabled={selectedIdx <= 0}
          onclick={() => selectedItem && void reorder(selectedItem, "up")}>{@render icoUp()}</button
        >
        <button
          class="tool-btn"
          title="Move down"
          disabled={selectedIdx < 0 || selectedIdx >= items.length - 1}
          onclick={() => selectedItem && void reorder(selectedItem, "down")}>{@render icoDown()}</button
        >
      </div>
    </div>
  </div>

  <footer class="foot">
    <span class="dot" style:background={STATE_COLOR[liveState]} title={liveState}></span>
    <span class="foot-name">{canvasName}</span>
    <button class="foot-gear" title="Edit canvas (Settings)" onclick={() => openSettings("canvases", canvasUuid)}
      >{@render icoGear()}</button
    >
  </footer>
</div>

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

  /* ---- embedded scenes / sources lists --------------------------------- */
  .embed {
    flex: 0 0 154px;
    display: flex;
    border-top: var(--border-weight) solid var(--color-border);
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
    flex: 0 0 42%;
    border-right: var(--border-weight) solid var(--color-border);
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
    font-size: 10px;
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

  /* ---- per-list toolbars ----------------------------------------------- */
  .toolbar {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 2px;
    padding: 4px 6px;
    border-top: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
  }
  .tool-spacer {
    flex: 1;
  }
  .tool-btn {
    width: 25px;
    height: 22px;
    display: flex;
    align-items: center;
    justify-content: center;
    background: none;
    border: 0;
    padding: 0;
    cursor: pointer;
    color: var(--color-dim);
  }
  .tool-btn:hover:not(:disabled) {
    color: var(--color-accent);
  }
  .tool-btn:disabled {
    opacity: 0.35;
    cursor: default;
  }

  /* ---- footer ---------------------------------------------------------- */
  .foot {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 5px 9px;
    border-top: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
  }
  .dot {
    width: 8px;
    height: 8px;
    flex-shrink: 0;
  }
  .foot-name {
    flex: 1;
    font-size: 10px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .foot-gear {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    justify-content: center;
    width: 22px;
    height: 20px;
    background: none;
    border: 0;
    padding: 0;
    cursor: pointer;
    color: var(--color-muted);
  }
  .foot-gear:hover {
    color: var(--color-accent);
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
