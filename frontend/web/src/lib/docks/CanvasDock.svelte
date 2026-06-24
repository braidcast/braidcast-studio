<script lang="ts">
  import { onMount } from "svelte";
  import { obs, type SceneInfo, type SceneItem, type ReorderDirection, type MultistreamState } from "../bridge";
  import { openSettings } from "../settingsOpener.svelte";
  import { previewSuspended, suspendPreview } from "../previewGate.svelte";
  import { WINDOW_ID } from "../windowContext";
  import ContextMenu, { type ContextMenuItem } from "../ContextMenu.svelte";
  import { openFilters } from "../filterDialogOpener.svelte";
  import { openTransform } from "../transformOpener.svelte";
  import { prefetchMonitors, projectorItems } from "../projectorMenu";

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

  function reportRect() {
    if (!previewEl) {
      return;
    }
    const r = previewEl.getBoundingClientRect();
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
    menu = {
      x: e.clientX,
      y: e.clientY,
      items: [
        { label: "Rename", action: () => beginRenameScene(name) },
        null,
        { label: "Remove", danger: true, disabled: scenes.length <= 1, action: () => removeScene(name) },
      ],
    };
  }

  // ---- sources (current scene of this canvas) --------------------------------
  let items = $state<SceneItem[]>([]);
  let selectedId = $state<number | null>(null);

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

  function openSourceMenu(e: MouseEvent, item: SceneItem, idx: number) {
    e.preventDefault();
    menu = {
      x: e.clientX,
      y: e.clientY,
      items: [
        { label: "Filters", disabled: !item.source, action: () => item.source && openFilters(item.source) },
        {
          label: "Edit Transform",
          action: () =>
            openTransform(
              { canvas: canvasUuid, scene: currentScene ?? undefined, id: item.id },
              item.source ?? "(unnamed)",
            ),
        },
        { label: "Rename", action: () => beginRenameSource(item) },
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
  function buildPreviewItems(p: {
    scene: string | null;
    id: number | null;
    source: string | null;
    visible: boolean;
    locked: boolean;
  }): (ContextMenuItem | null)[] {
    const call = (method: string, params: Record<string, unknown>) =>
      obs.call(method, { canvas: canvasUuid, scene: p.scene, id: p.id, ...params }).catch(report);
    return [
      {
        label: "Edit Transform",
        action: () =>
          p.id != null &&
          openTransform({ canvas: canvasUuid, scene: p.scene ?? undefined, id: p.id }, p.source ?? "(unnamed)"),
      },
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
    void loadScenes();
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
      }
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

    // Right-click in this canvas's overlay: filter to our uuid in this window with
    // a real hit, then map the device-px cursor to viewport coords via the rect.
    const offMenu = obs.on("preview.contextMenu", (p) => {
      if (p.canvas !== canvasUuid || p.window !== WINDOW_ID || p.id == null || !previewEl) {
        return;
      }
      const r = previewEl.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      menu = { x: r.left + p.x / dpr, y: r.top + p.y / dpr, items: buildPreviewItems(p), suspendOverlay: true };
    });

    return () => {
      ro.disconnect();
      window.removeEventListener("resize", reportRect);
      window.removeEventListener("scroll", reportRect, true);
      offScenes();
      offItems();
      offSel();
      offStatus();
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

<div class="dock-body">
  <div class="preview" bind:this={previewEl}>
    <span class="cap">{canvasName}</span>
  </div>

  {#if error}
    <p class="dock-msg err">{error}</p>
  {/if}

  <div class="lists">
    <div class="col">
      <div class="col-head">Scenes</div>
      <ul class="list">
        {#each scenes as scene (scene.name)}
          <li class="dock-row" class:sel={scene.current} oncontextmenu={(e) => openSceneMenu(e, scene.name)}>
            {#if renamingScene === scene.name}
              <input
                class="inline"
                bind:value={renameSceneTo}
                onkeydown={onRenameSceneKey}
                onblur={commitRenameScene}
                use:focusOnMount
              />
            {:else}
              <button class="dock-label" ondblclick={() => beginRenameScene(scene.name)} onclick={() => setCurrentScene(scene.name)}>{scene.name}</button>
            {/if}
          </li>
        {/each}
        {#if loaded && scenes.length === 0}
          <li class="dock-row dim">No scenes</li>
        {/if}
      </ul>
    </div>

    <div class="col">
      <div class="col-head">Sources</div>
      <ul class="list">
        {#each items as item, idx (item.id)}
          <li
            class="dock-row"
            class:sel={item.id === selectedId}
            class:dimmed={!item.visible}
            oncontextmenu={(e) => openSourceMenu(e, item, idx)}
          >
            <button class="dock-icon" title={item.visible ? "Hide" : "Show"} onclick={() => void toggleVisible(item)}>
              {item.visible ? "👁" : "🚫"}
            </button>
            <button class="dock-icon" title={item.locked ? "Unlock" : "Lock"} onclick={() => void toggleLocked(item)}>
              {item.locked ? "🔒" : "🔓"}
            </button>
            {#if renamingId === item.id}
              <input
                class="inline"
                bind:value={renameTo}
                onkeydown={onRenameSourceKey}
                onblur={commitRenameSource}
                use:focusOnMount
              />
            {:else}
              <button class="dock-label" onclick={() => selectItem(item)}>{item.source ?? "(unnamed)"}</button>
            {/if}
            <span class="dock-actions">
              <button class="dock-icon" title="Move up" disabled={idx === 0} onclick={() => void reorder(item, "up")}>▲</button
              >
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
        {#if currentScene && items.length === 0}
          <li class="dock-row dim">No sources</li>
        {/if}
      </ul>
    </div>
  </div>

  <footer class="foot">
    <span class="dot" style:background={STATE_COLOR[liveState]} title={liveState}></span>
    <span class="foot-name">{canvasName}</span>
    <button class="dock-icon gear" title="Edit canvas (Settings)" onclick={() => openSettings("canvases", canvasUuid)}>⚙</button
    >
  </footer>
</div>

{#if menu}
  <ContextMenu x={menu.x} y={menu.y} items={menu.items} onClose={() => (menu = null)} />
{/if}

<style>
  /* Override the shared .dock-body: this composite owns its inner scroll regions,
     so the body itself must not scroll (shared default sets overflow: auto). */
  .dock-body {
    min-height: 0;
    overflow: visible;
  }
  .preview {
    flex: 0 0 auto;
    height: 38%;
    min-height: 90px;
    position: relative;
    background: transparent; /* native overlay paints through */
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .cap {
    position: absolute;
    top: 6px;
    left: 8px;
    font-size: 9px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-muted);
    pointer-events: none;
  }
  .lists {
    flex: 1;
    min-height: 0;
    display: flex;
  }
  .col {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    overflow: auto;
    border-right: var(--border-weight) solid var(--color-border);
  }
  .col:last-child {
    border-right: none;
  }
  .col-head {
    flex: 0 0 auto;
    padding: 4px 7px;
    font-size: 9px;
    letter-spacing: var(--letter-spacing);
    text-transform: uppercase;
    color: var(--color-accent);
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .list {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  /* Compact overrides on the shared dock list classes: this dock is denser
     (10px labels, 4px padding) than the shared defaults. Each rule tweaks only
     the properties that differ; structure/selection comes from app.css. */
  .dock-row {
    padding: 4px 6px;
    gap: 3px;
  }
  .dock-label {
    font-size: 10px;
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
  .dock-icon {
    padding: 1px 3px;
    font-size: 10px;
  }
  .dock-actions {
    gap: 1px;
  }
  /* "No scenes / No sources" placeholder row (not a selectable list item). */
  .dock-row.dim {
    color: var(--color-muted);
    font-size: 10px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  .foot {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 5px 8px;
    border-top: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
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
  .gear {
    font-size: 12px;
  }
  /* This dock only ever shows an error message; keep the original tight pad/size
     and no letter-spacing (shared .dock-msg is roomier and tracks the token). */
  .dock-msg {
    padding: 6px 7px;
    font-size: 10px;
    letter-spacing: normal;
  }
</style>
