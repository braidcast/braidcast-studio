<script lang="ts">
  import { onMount } from "svelte";
  import { obs, type SceneInfo, type SceneItem, type ReorderDirection, type MultistreamState } from "../bridge";
  import { openSettings } from "../settingsOpener.svelte";
  import { previewSuspended } from "../previewGate.svelte";

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
        x: r.left,
        y: r.top,
        w: r.width,
        h: r.height,
        dpr: window.devicePixelRatio || 1,
      })
      .catch((e) => console.log("preview.setRect failed: " + (e as Error).message));
  }
  function hidePreview() {
    obs.call("preview.hide", { canvas: canvasUuid }).catch(() => {});
  }
  function destroyPreview() {
    obs.call("preview.destroy", { canvas: canvasUuid }).catch(() => {});
  }

  // ---- scenes (this canvas's own scene list) ---------------------------------
  let scenes = $state<SceneInfo[]>([]);
  let currentScene = $state<string | null>(null);

  async function loadScenes() {
    try {
      const list = await obs.call("scenes.list", { canvas: canvasUuid });
      scenes = list;
      currentScene = list.find((s) => s.current)?.name ?? null;
      error = null;
    } catch (e) {
      report(e);
    }
  }
  function setCurrentScene(name: string) {
    if (name === currentScene) {
      return;
    }
    obs.call("scenes.setCurrent", { canvas: canvasUuid, name }).catch(report);
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
    void obs.call("preview.select", { canvas: canvasUuid, scene: currentScene, id: item.id });
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
      if (!p.scene || p.scene === currentScene) {
        selectedId = p.id;
      }
    });
    const offStatus = obs.on("multistream.changed", (p) => recomputeState(p.outputs));

    return () => {
      ro.disconnect();
      window.removeEventListener("resize", reportRect);
      window.removeEventListener("scroll", reportRect, true);
      offScenes();
      offItems();
      offSel();
      offStatus();
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
</script>

<div class="canvas-dock">
  <div class="preview" bind:this={previewEl}>
    <span class="cap">{canvasName}</span>
  </div>

  {#if error}
    <p class="msg err">{error}</p>
  {/if}

  <div class="lists">
    <div class="col">
      <div class="col-head">Scenes</div>
      <ul class="list">
        {#each scenes as scene (scene.name)}
          <li class="row" class:sel={scene.current}>
            <button class="label" onclick={() => setCurrentScene(scene.name)}>{scene.name}</button>
          </li>
        {/each}
        {#if scenes.length === 0}
          <li class="row dim">No scenes</li>
        {/if}
      </ul>
    </div>

    <div class="col">
      <div class="col-head">Sources</div>
      <ul class="list">
        {#each items as item, idx (item.id)}
          <li class="row" class:sel={item.id === selectedId} class:hidden-src={!item.visible}>
            <button class="icon" title={item.visible ? "Hide" : "Show"} onclick={() => void toggleVisible(item)}>
              {item.visible ? "👁" : "🚫"}
            </button>
            <button class="icon" title={item.locked ? "Unlock" : "Lock"} onclick={() => void toggleLocked(item)}>
              {item.locked ? "🔒" : "🔓"}
            </button>
            <button class="label" onclick={() => selectItem(item)}>{item.source ?? "(unnamed)"}</button>
            <span class="actions">
              <button class="icon" title="Move up" disabled={idx === 0} onclick={() => void reorder(item, "up")}>▲</button
              >
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
        {#if currentScene && items.length === 0}
          <li class="row dim">No sources</li>
        {/if}
      </ul>
    </div>
  </div>

  <footer class="foot">
    <span class="dot" style:background={STATE_COLOR[liveState]} title={liveState}></span>
    <span class="foot-name">{canvasName}</span>
    <button class="icon gear" title="Edit canvas (Settings)" onclick={() => openSettings("canvases", canvasUuid)}>⚙</button
    >
  </footer>
</div>

<style>
  .canvas-dock {
    height: 100%;
    display: flex;
    flex-direction: column;
    background: var(--color-surface);
    font-family: var(--font-ui);
    min-height: 0;
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
  .row {
    display: flex;
    align-items: center;
    gap: 3px;
    padding: 4px 6px;
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
  .row.dim {
    color: var(--color-muted);
    font-size: 10px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
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
    font-size: 10px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    cursor: pointer;
  }
  .actions {
    display: none;
    gap: 1px;
  }
  .row:hover .actions {
    display: inline-flex;
  }
  .icon {
    background: none;
    border: none;
    height: auto;
    padding: 1px 3px;
    font-size: 10px;
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
  .msg {
    margin: 0;
    padding: 6px 7px;
    font-size: 10px;
    color: var(--color-live);
  }
</style>
