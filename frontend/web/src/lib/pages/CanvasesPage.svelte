<script lang="ts">
  import {
    obs,
    type CanvasInfo,
    type EncoderType,
    type OutputBindingInfo,
    type StreamProfileInfo,
    type MultistreamStatus,
    type MultistreamState,
  } from "../bridge";
  import CanvasEditor from "../CanvasEditor.svelte";
  import CollectionDialog, { type DialogSpec } from "../CollectionDialog.svelte";
  import { suspendPreview } from "../previewGate.svelte";

  // Canvases page: a gallery of large canvas tiles (an outline aspect-ratio preview
  // box per canvas). Clicking a tile opens a modal with that canvas's editor + its
  // destinations. Going live is the Studio GO-LIVE bar, never per-row here.
  let canvases = $state<CanvasInfo[]>([]);
  let bindings = $state<OutputBindingInfo[]>([]);
  let profiles = $state<StreamProfileInfo[]>([]);
  let live = $state<MultistreamStatus[]>([]);
  let videoEncoders = $state<EncoderType[]>([]);
  let audioEncoders = $state<EncoderType[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  // The canvas whose editor modal is open (null => the gallery grid).
  let editingUuid = $state<string | null>(null);

  // Per-canvas current scene name, for the tile's scene pill.
  let sceneByCanvas = $state<Record<string, string>>({});

  // Add-canvas name prompt (reuses the Studio inline canvas.create flow).
  let dialog = $state<DialogSpec | null>(null);

  // Inline "bind destination" picker inside the modal.
  let adding = $state(false);
  let addProfile = $state("");

  async function loadCanvases(): Promise<void> {
    try {
      canvases = await obs.call("canvas.list");
      void loadScenes();
    } catch (e) {
      error = (e as Error).message;
    }
  }
  function loadBindings(): void {
    obs
      .call("outputBinding.list")
      .then((l) => (bindings = l))
      .catch(() => {});
  }
  function loadProfiles(): void {
    obs
      .call("streamProfile.list")
      .then((l) => (profiles = l))
      .catch(() => {});
  }
  function loadLive(): void {
    obs
      .call("multistream.status")
      .then((r) => (live = r.outputs))
      .catch(() => {});
  }

  // Each canvas's current scene name (Default reads the global scenes; additional
  // canvases carry their uuid). Best-effort per canvas; blanks fall back to the name.
  async function loadScenes(): Promise<void> {
    const list = canvases;
    const entries = await Promise.all(
      list.map(async (c) => {
        try {
          const scenes = await obs.call("scenes.list", c.isDefault ? {} : { canvas: c.uuid });
          return [c.uuid, scenes.find((s) => s.current)?.name ?? ""] as const;
        } catch {
          return [c.uuid, ""] as const;
        }
      }),
    );
    sceneByCanvas = Object.fromEntries(entries);
  }

  async function loadAll(): Promise<void> {
    error = null;
    try {
      const [list, venc, aenc, binds, profs, status] = await Promise.all([
        obs.call("canvas.list"),
        obs.call("encoderTypes.list", { kind: "video" }),
        obs.call("encoderTypes.list", { kind: "audio" }),
        obs.call("outputBinding.list"),
        obs.call("streamProfile.list"),
        obs.call("multistream.status"),
      ]);
      canvases = list;
      videoEncoders = venc;
      audioEncoders = aenc;
      bindings = binds;
      profiles = profs;
      live = status.outputs;
      void loadScenes();
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void loadAll();
    const offCanvas = obs.on("canvas.changed", () => void loadCanvases());
    const offScenes = obs.on("scenes.changed", () => void loadScenes());
    const offBindings = obs.on("outputBinding.changed", () => {
      loadBindings();
      loadLive();
    });
    const offMulti = obs.on("multistream.changed", (p) => (live = p.outputs));
    const offProfiles = obs.on("streamProfile.changed", loadProfiles);
    return () => {
      offCanvas();
      offScenes();
      offBindings();
      offMulti();
      offProfiles();
    };
  });

  // Suspend the native preview overlay while the modal is open so it can never paint
  // over the dialog (defensive: the overlay is already gated off the Canvases page).
  $effect(() => {
    if (editingUuid !== null) {
      return suspendPreview();
    }
  });

  const editingCanvas = $derived(editingUuid ? (canvases.find((c) => c.uuid === editingUuid) ?? null) : null);
  const canvasBindings = $derived(bindings.filter((b) => b.canvasUuid === editingUuid));

  // bindingUuid -> live status row.
  const statusByBinding = $derived.by<Map<string, MultistreamStatus>>(() => {
    const m = new Map<string, MultistreamStatus>();
    for (const o of live) {
      m.set(o.bindingUuid, o);
    }
    return m;
  });

  function fpsText(c: CanvasInfo): string {
    if (!(c.fpsDen > 0)) return String(c.fpsNum);
    return c.fpsDen > 1 ? (c.fpsNum / c.fpsDen).toFixed(2) : String(c.fpsNum);
  }
  function isVertical(c: CanvasInfo): boolean {
    return c.outputHeight > c.outputWidth;
  }
  function encName(list: EncoderType[], id: string): string {
    return list.find((e) => e.id === id)?.name ?? id ?? "—";
  }

  // Per-canvas rollup for the tile footer: how many destinations, and the strongest
  // live state across its enabled bindings.
  function canvasDests(uuid: string): { enabled: number; total: number } {
    const rows = bindings.filter((b) => b.canvasUuid === uuid);
    return { enabled: rows.filter((b) => b.enabled).length, total: rows.length };
  }
  function canvasState(uuid: string): MultistreamState | "off" {
    const rows = bindings.filter((b) => b.canvasUuid === uuid && b.enabled);
    if (rows.length === 0) return "off";
    const states = rows.map((b) => statusByBinding.get(b.uuid)?.state ?? "idle");
    if (states.includes("live")) return "live";
    if (states.includes("error")) return "error";
    if (states.includes("connecting")) return "connecting";
    return "idle";
  }
  const STATE_COLOR: Record<MultistreamState | "off" | "disabled", string> = {
    off: "var(--color-muted)",
    disabled: "var(--color-muted)",
    idle: "var(--color-muted)",
    connecting: "var(--meter-yellow)",
    live: "var(--meter-green)",
    error: "var(--color-live)",
  };
  const STATE_TAG_BG: Record<MultistreamState | "disabled", string> = {
    disabled: "color-mix(in srgb, var(--color-muted) 10%, transparent)",
    idle: "color-mix(in srgb, var(--color-muted) 12%, transparent)",
    connecting: "color-mix(in srgb, var(--meter-yellow) 14%, transparent)",
    live: "color-mix(in srgb, var(--meter-green) 14%, transparent)",
    error: "color-mix(in srgb, var(--color-live) 14%, transparent)",
  };
  function titleState(s: string): string {
    return s.charAt(0).toUpperCase() + s.slice(1);
  }

  // The effective state of a destination row: disabled bindings never go live.
  function rowState(b: OutputBindingInfo): MultistreamState | "disabled" {
    if (!b.enabled) {
      return "disabled";
    }
    return statusByBinding.get(b.uuid)?.state ?? "idle";
  }
  function isDangling(label: string): boolean {
    return label === "(deleted)";
  }
  function isUnset(label: string): boolean {
    return label === "(unset)";
  }
  function rowName(b: OutputBindingInfo): string {
    if (isUnset(b.profileLabel)) {
      return "No destination";
    }
    return b.profileLabel;
  }

  // Per-canvas enabled = any binding enabled; the master toggle sets them all.
  const canvasEnabled = $derived(canvasBindings.some((b) => b.enabled));
  async function toggleCanvas(): Promise<void> {
    const rows = canvasBindings;
    if (rows.length === 0) return;
    const target = !canvasEnabled;
    try {
      await Promise.all(rows.map((b) => obs.call("outputBinding.setEnabled", { uuid: b.uuid, enabled: target })));
      loadBindings();
    } catch (e) {
      error = (e as Error).message;
    }
  }
  async function toggleRow(b: OutputBindingInfo, enabled: boolean): Promise<void> {
    try {
      await obs.call("outputBinding.setEnabled", { uuid: b.uuid, enabled });
      loadBindings();
    } catch (e) {
      error = (e as Error).message;
    }
  }
  async function removeRow(b: OutputBindingInfo): Promise<void> {
    try {
      await obs.call("outputBinding.remove", { uuid: b.uuid });
      loadBindings();
    } catch (e) {
      error = (e as Error).message;
    }
  }

  function startAdd(): void {
    adding = true;
    addProfile = profiles[0]?.uuid ?? "";
  }
  function cancelAdd(): void {
    adding = false;
  }
  async function confirmAdd(): Promise<void> {
    if (!editingUuid) return;
    try {
      await obs.call("outputBinding.create", {
        canvasUuid: editingUuid,
        ...(addProfile ? { profileUuid: addProfile } : {}),
      });
      adding = false;
      loadBindings();
    } catch (e) {
      error = (e as Error).message;
    }
  }

  function openEditor(uuid: string): void {
    adding = false;
    editingUuid = uuid;
  }
  function closeEditor(): void {
    editingUuid = null;
    adding = false;
  }

  // Inline add-canvas: a name prompt -> canvas.create seeded with the Default
  // canvas's resolution/FPS. The new canvas appears via canvas.changed; open it.
  function addCanvas(): void {
    dialog = {
      kind: "prompt",
      title: "New Canvas",
      confirmLabel: "Create",
      onCommit: (name) => {
        if (!name) return;
        const def = canvases.find((c) => c.isDefault);
        obs
          .call("canvas.create", {
            name,
            baseWidth: def?.baseWidth ?? 1920,
            baseHeight: def?.baseHeight ?? 1080,
            outputWidth: def?.outputWidth,
            outputHeight: def?.outputHeight,
            fpsNum: def?.fpsNum ?? 60,
            fpsDen: def?.fpsDen ?? 1,
          })
          .then((r) => (editingUuid = r.uuid))
          .catch(() => {});
      },
    };
  }

  function noop(): void {}
</script>

<svelte:window
  onkeydown={(e) => {
    if (e.key === "Escape" && editingUuid !== null) closeEditor();
  }}
/>

<div class="page">
  <header class="head">
    <div class="head-titles">
      <span class="title">Canvases</span>
      <span class="sub">encode targets · one per resolution/FPS</span>
    </div>
    {#if loaded}
      <span class="head-count">{canvases.length} canvas{canvases.length === 1 ? "" : "es"}</span>
    {/if}
  </header>

  <div class="body">
    {#if error}<p class="err">{error}</p>{/if}

    {#if !loaded}
      <p class="dim">Loading canvases…</p>
    {:else}
      <div class="grid">
        {#each canvases as c (c.uuid)}
          {@const st = canvasState(c.uuid)}
          {@const d = canvasDests(c.uuid)}
          <button class="tile" onclick={() => openEditor(c.uuid)}>
            <div class="media">
              <div class="stage" class:vertical={isVertical(c)}>
                <span class="res-chip">{c.outputWidth} × {c.outputHeight}</span>
                {#if st === "live"}<span class="live-chip">● LIVE</span>{/if}
                <div class="scene-tag">
                  <span class="scene-bar"></span>
                  <span class="scene-pill">{sceneByCanvas[c.uuid] || c.name}</span>
                </div>
              </div>
            </div>
            <div class="tile-info">
              <div class="tile-foot">
                <span class="tile-name">{c.name}</span>
                {#if c.isDefault}<span class="badge">Default</span>{/if}
                <span class="spacer"></span>
                <span class="tile-fps">{fpsText(c)} fps</span>
              </div>
              <div class="tile-status">
                <span class="dot" style:background={STATE_COLOR[st]}></span>
                <span class="tile-dests">
                  {#if d.total === 0}
                    No destinations
                  {:else}
                    {d.enabled}/{d.total} destination{d.total === 1 ? "" : "s"}
                  {/if}
                </span>
              </div>
            </div>
          </button>
        {/each}

        <button class="tile add-tile" onclick={addCanvas}>
          <span class="add-plus">＋</span>
          <span>Add canvas</span>
        </button>
      </div>
    {/if}
  </div>
</div>

{#if editingCanvas}
  <div
    class="modal-scrim"
    role="presentation"
    onclick={(e) => {
      if (e.target === e.currentTarget) closeEditor();
    }}
  >
    <div class="modal" role="dialog" aria-modal="true" aria-label="Edit canvas" tabindex="-1">
      <header class="modal-head">
        <span class="modal-title">{editingCanvas.name}</span>
        {#if editingCanvas.isDefault}<span class="badge">Default</span>{/if}
        <span class="spacer"></span>
        <button class="modal-x" title="Close" aria-label="Close" onclick={closeEditor}>
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round">
            <path d="M6 6l12 12M18 6L6 18" />
          </svg>
        </button>
      </header>

      <div class="modal-body">
        <section class="section">
          <div class="sec-bar">
            <h3 class="sec-head">Settings</h3>
          </div>
          <p class="sec-desc">Resolution, frame rate, and encoders for this encode target.</p>
          <CanvasEditor
            canvas={editingCanvas}
            {videoEncoders}
            {audioEncoders}
            embedded
            onClose={noop}
            onSaved={() => {
              void loadCanvases();
              closeEditor();
            }}
          />
        </section>

        <section class="section">
          <div class="sec-bar">
            <h3 class="sec-head">Destinations</h3>
            <span class="sec-count"
              >{canvasBindings.filter((b) => b.enabled).length}/{canvasBindings.length} enabled</span
            >
            <span class="spacer"></span>
            {#if canvasBindings.length > 0}
              <label class="switch" title={canvasEnabled ? "Disable all" : "Enable all"}>
                <input type="checkbox" checked={canvasEnabled} onchange={() => void toggleCanvas()} />
                <span class="track"><span class="thumb2"></span></span>
              </label>
            {/if}
          </div>

          {#if canvasBindings.length === 0 && !adding}
            <p class="empty">No destinations bound to this canvas yet.</p>
          {/if}

          <div class="rows">
            {#each canvasBindings as b (b.uuid)}
              {@const s = rowState(b)}
              <div class="row" class:off={!b.enabled}>
                <label class="switch sm" title={b.enabled ? "Disable" : "Enable"}>
                  <input type="checkbox" checked={b.enabled} onchange={(e) => void toggleRow(b, e.currentTarget.checked)} />
                  <span class="track"><span class="thumb2"></span></span>
                </label>
                <div class="row-col">
                  <div class="row-line1">
                    <span class="row-name" class:deleted={isDangling(b.profileLabel)} class:unset={isUnset(b.profileLabel)}>
                      {rowName(b)}
                    </span>
                    <span class="row-state" style:color={STATE_COLOR[s]} style:background={STATE_TAG_BG[s]}>
                      {titleState(s).toUpperCase()}
                    </span>
                  </div>
                  <div class="row-sub">{encName(videoEncoders, editingCanvas.videoEncoder)}</div>
                </div>
                <button class="trash" title="Remove destination" aria-label="Remove destination" onclick={() => void removeRow(b)}>
                  <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round">
                    <path d="M4 7h16M9 7V5h6v2M6 7l1 13h10l1-13" />
                  </svg>
                </button>
              </div>
            {/each}

            {#if adding}
              <div class="row add-form">
                <span class="add-label">New destination</span>
                <select bind:value={addProfile}>
                  <option value="">No destination (placeholder)</option>
                  {#each profiles as p (p.uuid)}
                    <option value={p.uuid}>{p.label || p.platform}</option>
                  {/each}
                </select>
                <div class="add-actions">
                  <button class="ghost" onclick={cancelAdd}>Cancel</button>
                  <button class="accent" onclick={() => void confirmAdd()}>Add</button>
                </div>
              </div>
            {:else}
              <button class="add-tile-row" onclick={startAdd}>
                <span class="add-plus">＋</span>
                <span>Bind destination</span>
              </button>
            {/if}
          </div>
        </section>
      </div>
    </div>
  </div>
{/if}

{#if dialog}
  <CollectionDialog {...dialog} onClose={() => (dialog = null)} />
{/if}

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
    background: var(--color-base);
    color: var(--color-text);
  }
  .head {
    flex: 0 0 auto;
    height: 58px;
    display: flex;
    align-items: center;
    gap: 16px;
    padding: 0 24px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .head-titles {
    display: flex;
    align-items: baseline;
    gap: 12px;
    min-width: 0;
  }
  .title {
    font-family: var(--font-ui);
    font-size: 16px;
    font-weight: 600;
    letter-spacing: -0.01em;
  }
  .sub {
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
  }
  .head-count {
    margin-left: auto;
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    color: var(--color-muted);
  }

  .body {
    flex: 1;
    min-height: 0;
    overflow: auto;
    padding: 22px 24px 32px;
  }

  /* ---- tile gallery ------------------------------------------------------ */
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
    gap: 18px;
    align-content: start;
  }
  .tile {
    display: flex;
    flex-direction: column;
    text-align: left;
    padding: 0;
    height: auto;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    cursor: pointer;
    transition:
      border-color 0.12s ease,
      transform 0.12s ease;
  }
  .tile:hover {
    border-color: color-mix(in srgb, var(--color-accent) 60%, var(--color-border));
    transform: translateY(-2px);
  }
  .tile:focus-visible {
    outline: var(--border-weight) solid var(--color-accent);
    outline-offset: 2px;
  }

  /* Representational preview: ONE block-level aspect box (the .stage), framed by a
     padded media band on the base color. No native video here -- text chips only. */
  /* Fixed-height band for BOTH orientations: the stage takes a definite height from
     the band and derives its WIDTH via aspect-ratio. Deriving height from width:100%
     is not honored by this Chromium build, so we never rely on it. */
  .media {
    position: relative;
    height: 200px;
    padding: 16px;
    box-sizing: border-box;
    display: flex;
    align-items: center;
    justify-content: center;
    background: var(--color-base);
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .stage {
    position: relative;
    height: 100%;
    aspect-ratio: 16 / 9;
    max-width: 100%;
    background: color-mix(in srgb, var(--color-text) 2.5%, transparent);
    box-shadow: 0 0 0 1px var(--color-border);
  }
  .stage.vertical {
    aspect-ratio: 9 / 16;
  }
  .res-chip {
    position: absolute;
    left: 8px;
    top: 8px;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    color: rgba(255, 255, 255, 0.5);
    border: var(--border-weight) solid rgba(255, 255, 255, 0.16);
    padding: 2px 6px;
  }
  .live-chip {
    position: absolute;
    right: 8px;
    top: 8px;
    font-family: var(--font-mono);
    font-size: 9px;
    color: #fff;
    background: var(--color-live);
    padding: 2px 6px;
  }
  .scene-tag {
    position: absolute;
    left: 0;
    bottom: 0;
    display: flex;
    align-items: stretch;
    max-width: 100%;
  }
  .scene-bar {
    width: 4px;
    background: var(--color-accent);
  }
  .scene-pill {
    background: rgba(8, 8, 10, 0.78);
    padding: 5px 10px;
    font-size: 11px;
    font-weight: 600;
    color: #fff;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .tile-info {
    padding: 12px 13px 13px;
  }
  .tile-foot {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .tile-name {
    font-family: var(--font-ui);
    font-size: 14px;
    font-weight: 600;
    letter-spacing: -0.01em;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .tile-fps {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .tile-status {
    display: flex;
    align-items: center;
    gap: 7px;
    margin-top: 8px;
  }
  .tile-status .dot {
    width: 7px;
    height: 7px;
    flex: 0 0 auto;
  }
  .tile-dests {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
    letter-spacing: 0.02em;
  }

  .add-tile {
    align-items: center;
    justify-content: center;
    gap: 8px;
    min-height: 190px;
    border-style: dashed;
    background: transparent;
    color: var(--color-dim);
    font-family: var(--font-ui);
    font-size: 13px;
  }
  .add-tile:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
    transform: none;
  }
  .add-plus {
    font-size: 15px;
    line-height: 1;
  }

  .badge {
    font-family: var(--font-mono);
    font-size: 8px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--color-accent-ink);
    background: var(--color-accent);
    padding: 2px 5px;
    flex: 0 0 auto;
  }
  .spacer {
    flex: 1;
  }
  .dim {
    color: var(--color-muted);
    margin: 0;
  }
  .err {
    margin: 0 0 12px;
    color: var(--color-live);
    font-size: 12px;
  }

  /* ---- modal ------------------------------------------------------------- */
  .modal-scrim {
    position: fixed;
    inset: 0;
    z-index: 60;
    display: flex;
    align-items: flex-start;
    justify-content: center;
    padding: 48px 24px;
    background: rgba(0, 0, 0, 0.55);
    overflow: auto;
  }
  .modal {
    width: min(680px, 100%);
    max-height: calc(100vh - 96px);
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    box-shadow: 0 24px 80px rgba(0, 0, 0, 0.6);
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .modal-head {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 14px 16px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .modal-title {
    font-family: var(--font-ui);
    font-size: 15px;
    font-weight: 600;
  }
  .modal-x {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 28px;
    height: 26px;
    background: none;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-muted);
    cursor: pointer;
  }
  .modal-x:hover {
    color: var(--color-text);
    border-color: var(--color-accent);
  }
  .modal-body {
    flex: 1;
    min-height: 0;
    padding: 20px 22px 26px;
    overflow: auto;
  }
  .section {
    margin-bottom: 28px;
  }
  .section:last-child {
    margin-bottom: 0;
  }
  .sec-head {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: var(--color-dim);
  }
  .sec-desc {
    margin: 6px 0 0;
    font-size: 12px;
    color: var(--color-muted);
  }
  .sec-count {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    color: var(--color-muted);
  }
  .sec-bar {
    display: flex;
    align-items: center;
    gap: 10px;
    padding-bottom: 10px;
    border-bottom: var(--border-weight) solid var(--color-border-2);
    margin-bottom: 12px;
  }
  .empty {
    margin: 4px 0 12px;
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
  }
  .rows {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 12px 14px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .row.off {
    background: var(--color-base);
  }
  .row-col {
    flex: 1;
    min-width: 0;
  }
  .row-line1 {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .row-name {
    font-size: 14px;
    font-weight: 500;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .row-name.deleted {
    color: var(--color-live);
    font-style: italic;
  }
  .row-name.unset {
    color: var(--color-muted);
    font-style: italic;
    font-weight: 400;
  }
  .row-state {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.06em;
    padding: 2px 6px;
  }
  .row-sub {
    margin-top: 3px;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .trash {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    justify-content: center;
    width: 28px;
    height: 26px;
    padding: 0;
    background: none;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-muted);
    cursor: pointer;
  }
  .trash:hover {
    color: var(--color-live);
    border-color: var(--color-live);
  }

  /* enable/disable switch (square thumb per the 0-radius rule). */
  .switch {
    flex: 0 0 auto;
    display: inline-flex;
    align-items: center;
    cursor: pointer;
  }
  .switch input {
    position: absolute;
    opacity: 0;
    width: 0;
    height: 0;
  }
  .track {
    display: block;
    width: 36px;
    height: 20px;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    position: relative;
    transition: background 0.12s ease;
  }
  .switch.sm .track {
    width: 32px;
    height: 18px;
  }
  .thumb2 {
    position: absolute;
    top: 50%;
    left: 3px;
    width: 12px;
    height: 12px;
    transform: translateY(-50%);
    background: var(--color-muted);
    transition:
      left 0.12s ease,
      background 0.12s ease;
  }
  .switch.sm .thumb2 {
    width: 10px;
    height: 10px;
  }
  .switch input:checked + .track {
    background: color-mix(in srgb, var(--color-accent) 30%, transparent);
    border-color: var(--color-accent);
  }
  .switch input:checked + .track .thumb2 {
    left: calc(100% - 12px - 3px);
    background: var(--color-accent);
  }
  .switch.sm input:checked + .track .thumb2 {
    left: calc(100% - 10px - 3px);
  }
  .switch input:focus-visible + .track {
    outline: 1px solid var(--color-accent);
    outline-offset: 1px;
  }

  .add-tile-row {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    padding: 12px 16px;
    border: var(--border-weight) dashed var(--color-border);
    background: transparent;
    color: var(--color-dim);
    cursor: pointer;
    font-family: var(--font-ui);
    font-size: 12px;
  }
  .add-tile-row:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .add-form {
    flex-direction: column;
    align-items: stretch;
    gap: 10px;
  }
  .add-label {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .add-form select {
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font: inherit;
    font-size: 13px;
    padding: 7px 10px;
  }
  .add-actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
  }
  .add-actions .ghost {
    padding: 7px 14px;
    background: none;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    cursor: pointer;
    font: inherit;
    font-size: 12px;
  }
  .add-actions .ghost:hover {
    color: var(--color-text);
  }
  .add-actions .accent {
    padding: 7px 16px;
    background: var(--color-accent);
    border: 0;
    color: var(--color-accent-ink);
    cursor: pointer;
    font: inherit;
    font-size: 12px;
    font-weight: 600;
  }
</style>
