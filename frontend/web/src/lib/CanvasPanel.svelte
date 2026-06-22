<script lang="ts">
  import { onMount } from "svelte";
  import { obs, type CanvasInfo } from "./bridge";
  import { previewSuspended } from "./previewGate.svelte";
  import { createCanvasSceneStore } from "./canvasScenes.svelte";
  import { canvasFocus, setFocusedCanvas } from "./canvasFocus.svelte";
  import { openSettings } from "./settingsOpener.svelte";
  import type { MultistreamStatus, MultistreamState } from "./bridge";

  interface Props {
    canvas: CanvasInfo;
    /** Live status of an output bound to this canvas, if any (for the dot). */
    status?: MultistreamStatus | null;
  }
  let { canvas, status = null }: Props = $props();

  // uuid/isDefault are stable for this instance: each panel is keyed by uuid in
  // CanvasPanels, so a canvas swap remounts the panel rather than mutating these.
  // svelte-ignore state_referenced_locally
  const uuid = canvas.uuid;
  // svelte-ignore state_referenced_locally
  const store = createCanvasSceneStore(uuid, canvas.isDefault);
  const sceneState = store.state;

  // Tall (portrait) panels stack scenes above preview; wide (landscape) put the
  // scene list beside the preview -- mirrors the responsive-dock mock.
  const portrait = $derived(canvas.baseHeight > canvas.baseWidth);

  const focused = $derived(canvasFocus.uuid === uuid);

  function focus() {
    if (!focused) setFocusedCanvas(uuid);
  }

  // --- per-canvas preview region (generalized PreviewArea) -------------------
  let previewEl = $state<HTMLElement | undefined>();

  function reportRect() {
    if (!previewEl) return;
    const r = previewEl.getBoundingClientRect();
    obs
      .call("preview.setRect", {
        canvas: uuid,
        x: r.left,
        y: r.top,
        w: r.width,
        h: r.height,
        dpr: window.devicePixelRatio || 1,
      })
      .catch((e) => console.log("preview.setRect failed: " + (e as Error).message));
  }

  function hidePreview() {
    obs.call("preview.hide", { canvas: uuid }).catch(() => {});
  }

  // On unmount (the canvas left the enabled set) fully tear the surface down, not
  // just hide it, so a disabled canvas's native display does not linger to shutdown.
  function destroyPreview() {
    obs.call("preview.destroy", { canvas: uuid }).catch(() => {});
  }

  onMount(() => {
    reportRect();
    const ro = new ResizeObserver(reportRect);
    if (previewEl) ro.observe(previewEl);
    window.addEventListener("resize", reportRect);
    window.addEventListener("scroll", reportRect, true);
    return () => {
      ro.disconnect();
      window.removeEventListener("resize", reportRect);
      window.removeEventListener("scroll", reportRect, true);
      destroyPreview();
      store.dispose();
    };
  });

  // The global previewGate suspends every native overlay while a modal is open;
  // each region hides ITS OWN surface and re-asserts its rect when cleared.
  $effect(() => {
    if (previewSuspended()) {
      hidePreview();
    } else {
      reportRect();
    }
  });

  // The portrait/landscape switch relays out our element; re-report afterwards.
  $effect(() => {
    void portrait;
    if (!previewSuspended()) queueMicrotask(reportRect);
  });

  // --- per-canvas scene list (canvas-scoped ScenesPanel logic) ---------------
  let adding = $state(false);
  let newName = $state("");
  let renamingFrom = $state<string | null>(null);
  let renameTo = $state("");
  let actionError = $state<string | null>(null);

  function focusOnMount(node: HTMLInputElement) {
    node.focus();
    node.select();
  }

  function reportError(e: unknown) {
    actionError = (e as Error).message;
  }

  async function setCurrent(name: string) {
    if (name === sceneState.current) return;
    actionError = null;
    try {
      await obs.call("scenes.setCurrent", { canvas: uuid, name });
    } catch (e) {
      reportError(e);
    }
  }

  function beginAdd() {
    actionError = null;
    adding = true;
    newName = "";
  }

  async function commitAdd() {
    const name = newName.trim();
    if (!name) {
      adding = false;
      return;
    }
    actionError = null;
    try {
      await obs.call("scenes.create", { canvas: uuid, name });
      await obs.call("scenes.setCurrent", { canvas: uuid, name });
      adding = false;
    } catch (e) {
      reportError(e);
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
    if (!from || !to || to === from) return;
    actionError = null;
    try {
      await obs.call("scenes.rename", { canvas: uuid, from, to });
    } catch (e) {
      reportError(e);
    }
  }

  async function remove(name: string) {
    actionError = null;
    try {
      await obs.call("scenes.remove", { canvas: uuid, name });
    } catch (e) {
      reportError(e);
    }
  }

  function onAddKey(e: KeyboardEvent) {
    if (e.key === "Enter") void commitAdd();
    else if (e.key === "Escape") adding = false;
  }

  function onRenameKey(e: KeyboardEvent) {
    if (e.key === "Enter") void commitRename();
    else if (e.key === "Escape") renamingFrom = null;
  }

  // Status dot color from the live state of a bound output.
  const dotState = $derived<MultistreamState | "off">(status?.state ?? "off");
</script>

<section
  class="canvas-panel"
  class:portrait
  class:focused
  role="group"
  aria-label={`Canvas ${canvas.name}`}
  onpointerdown={focus}
>
  <header class="panel-head">
    <div class="title">
      <span class="name">{canvas.name}</span>
      {#if canvas.isDefault}<span class="badge">Default</span>{/if}
      <span class="res">{canvas.baseWidth}×{canvas.baseHeight}</span>
    </div>
  </header>

  <div class="body">
    <div class="scenes">
      <div class="scenes-head">
        <span class="section-label">Scenes</span>
        <button class="icon" title="Add scene" onclick={beginAdd}>＋</button>
      </div>

      {#if sceneState.error}
        <p class="error">{sceneState.error}</p>
      {:else if !sceneState.loaded}
        <p class="dim">Loading…</p>
      {:else}
        <ul>
          {#each sceneState.scenes as scene (scene.name)}
            <li class:active={scene.name === sceneState.current}>
              {#if renamingFrom === scene.name}
                <input
                  class="inline-input"
                  bind:value={renameTo}
                  onkeydown={onRenameKey}
                  onblur={commitRename}
                  use:focusOnMount
                />
              {:else}
                <button
                  class="scene-name"
                  ondblclick={() => beginRename(scene.name)}
                  onclick={() => setCurrent(scene.name)}
                >
                  {scene.name}
                </button>
                <span class="row-actions">
                  <button class="icon" title="Rename" onclick={() => beginRename(scene.name)}>✎</button>
                  <button
                    class="icon"
                    title="Remove"
                    disabled={sceneState.scenes.length <= 1}
                    onclick={() => remove(scene.name)}>🗑</button
                  >
                </span>
              {/if}
            </li>
          {/each}

          {#if adding}
            <li>
              <input
                class="inline-input"
                placeholder="Scene name"
                bind:value={newName}
                onkeydown={onAddKey}
                onblur={commitAdd}
                use:focusOnMount
              />
            </li>
          {/if}
        </ul>

        {#if sceneState.scenes.length === 0 && !adding}
          <p class="dim">No scenes</p>
        {/if}
      {/if}

      {#if actionError}
        <p class="error">{actionError}</p>
      {/if}
    </div>

    <!-- The native preview overlay (a sibling HWND above CEF) covers this exact
         element; it stays transparent so the overlay paints through. -->
    <section class="preview" bind:this={previewEl}>
      <span class="label">{canvas.name}</span>
    </section>
  </div>

  <footer class="panel-foot">
    <div class="foot-left">
      <button
        class="icon"
        title="Canvas settings"
        aria-label="Canvas settings"
        onclick={() => openSettings("canvases", uuid)}>⚙</button
      >
    </div>
    <div class="foot-right">
      <span class="dot" data-state={dotState} title={status?.lastError || dotState}></span>
    </div>
  </footer>
</section>

<style>
  .canvas-panel {
    border: 1px solid var(--border);
    border-radius: 10px;
    background: var(--bg-raised);
    display: flex;
    flex-direction: column;
    min-height: 0;
    min-width: 0;
    overflow: hidden;
  }
  .canvas-panel.focused {
    border-color: var(--accent);
  }
  .panel-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    padding: 8px 12px;
    border-bottom: 1px solid var(--border);
    background: var(--bg-sunken);
  }
  .title {
    display: flex;
    align-items: center;
    gap: 8px;
    min-width: 0;
  }
  .dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    flex-shrink: 0;
    background: var(--text-dim);
  }
  .dot[data-state="off"] {
    background: var(--text-dim);
  }
  .dot[data-state="idle"] {
    background: var(--text-dim);
  }
  .dot[data-state="connecting"] {
    background: #e0b341;
  }
  .dot[data-state="live"] {
    background: var(--on, #4caf50);
    box-shadow: 0 0 6px var(--on, #4caf50);
  }
  .dot[data-state="error"] {
    background: var(--off, #d65a5a);
  }
  .name {
    font-size: 13px;
    font-weight: 600;
    color: var(--text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .badge {
    font-size: 9px;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: #fff;
    background: var(--accent);
    border-radius: 999px;
    padding: 1px 6px;
  }
  .res {
    font-size: 11px;
    color: var(--text-dim);
  }
  .body {
    flex: 1;
    display: flex;
    min-height: 0;
    min-width: 0;
  }
  .panel-foot {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 5px 10px;
    border-top: 1px solid var(--border);
    background: var(--bg-sunken);
  }
  .foot-left,
  .foot-right {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  /* Wide/landscape: scenes left of preview. */
  .canvas-panel:not(.portrait) .body {
    flex-direction: row;
  }
  .canvas-panel:not(.portrait) .scenes {
    width: 150px;
    border-right: 1px solid var(--border);
  }
  /* Tall/portrait: scenes above preview. */
  .canvas-panel.portrait .body {
    flex-direction: column;
  }
  .canvas-panel.portrait .scenes {
    border-bottom: 1px solid var(--border);
    max-height: 40%;
  }
  .scenes {
    display: flex;
    flex-direction: column;
    gap: 6px;
    padding: 8px 10px;
    overflow: auto;
    flex-shrink: 0;
  }
  .scenes-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  .section-label {
    font-size: 11px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-dim);
  }
  ul {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  li {
    display: flex;
    align-items: center;
    gap: 4px;
    padding: 3px 5px;
    border-radius: 6px;
  }
  li.active {
    background: color-mix(in srgb, var(--accent) 22%, transparent);
  }
  .scene-name {
    flex: 1;
    text-align: left;
    background: none;
    border: none;
    color: var(--text-soft);
    cursor: pointer;
    padding: 2px 3px;
    font: inherit;
    font-size: 12px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  li.active .scene-name {
    color: var(--text);
  }
  .row-actions {
    display: none;
    gap: 2px;
  }
  li:hover .row-actions {
    display: inline-flex;
  }
  .icon {
    background: none;
    border: none;
    color: var(--text-dim);
    cursor: pointer;
    padding: 2px 4px;
    border-radius: 4px;
    font-size: 12px;
    line-height: 1;
  }
  .icon:hover:not(:disabled) {
    color: var(--text);
    background: var(--bg-sunken);
  }
  .icon:disabled {
    opacity: 0.35;
    cursor: default;
  }
  .inline-input {
    flex: 1;
    background: var(--bg-sunken);
    border: 1px solid var(--accent);
    border-radius: 4px;
    color: var(--text);
    padding: 3px 5px;
    font: inherit;
    font-size: 12px;
  }
  .preview {
    flex: 1;
    min-height: 140px;
    min-width: 0;
    position: relative;
    /* Transparent: the native overlay HWND paints this region. */
    background: transparent;
    overflow: hidden;
  }
  .label {
    position: absolute;
    top: 6px;
    left: 8px;
    font-size: 10px;
    letter-spacing: 0.04em;
    text-transform: uppercase;
    color: var(--text-dim);
    pointer-events: none;
  }
  .dim {
    color: var(--text-dim);
    margin: 0;
    font-size: 12px;
  }
  .error {
    color: var(--off, #d65a5a);
    margin: 0;
    font-size: 12px;
  }
</style>
