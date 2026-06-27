<script lang="ts">
  import {
    obs,
    type CanvasInfo,
    type EncoderType,
    type CanvasCreateParams,
    type CanvasUpdateParams,
  } from "./bridge";
  import PropertyForm from "./properties/PropertyForm.svelte";

  interface Props {
    /** A canvas uuid to open for editing once the list loads (deep-link from a
     * canvas panel's settings button). */
    editCanvas?: string | null;
  }
  let { editCanvas = null }: Props = $props();

  // Resolution presets the form offers; custom values still accepted via inputs.
  const resPresets: { label: string; w: number; h: number }[] = [
    { label: "1920 × 1080", w: 1920, h: 1080 },
    { label: "1280 × 720", w: 1280, h: 720 },
    { label: "2560 × 1440", w: 2560, h: 1440 },
    { label: "3840 × 2160", w: 3840, h: 2160 },
  ];
  const fpsPresets = [24, 30, 48, 60];

  let canvases = $state<CanvasInfo[]>([]);
  let videoEncoders = $state<EncoderType[]>([]);
  let audioEncoders = $state<EncoderType[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  // Inline add/edit form. `editingUuid` null => the add form; a uuid => editing.
  let formOpen = $state(false);
  let editingUuid = $state<string | null>(null);
  let fName = $state("");
  let fWidth = $state(1920);
  let fHeight = $state(1080);
  let fFps = $state(60);
  let resCustom = $state(false);
  let fpsCustom = $state(false);
  let fVideoEnc = $state("");
  let fAudioEnc = $state("");
  let saving = $state(false);
  let formError = $state<string | null>(null);

  // Which canvas's encoder-settings expander is open (uuid), and which kind.
  let expandedUuid = $state<string | null>(null);
  let expandedKind = $state<"video" | "audio">("video");

  async function loadCanvases() {
    try {
      canvases = await obs.call("canvas.list");
    } catch (e) {
      error = (e as Error).message;
    }
  }

  async function loadAll() {
    error = null;
    try {
      const [list, venc, aenc] = await Promise.all([
        obs.call("canvas.list"),
        obs.call("encoderTypes.list", { kind: "video" }),
        obs.call("encoderTypes.list", { kind: "audio" }),
      ]);
      canvases = list;
      videoEncoders = venc;
      audioEncoders = aenc;
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void loadAll();
    // Live-refresh the list when any canvas mutates (this tab or elsewhere).
    const off = obs.on("canvas.changed", () => void loadCanvases());
    return off;
  });

  // Deep-link: when opened with an editCanvas uuid, open that canvas's edit form
  // once the list is loaded (runs once per uuid).
  let deepLinked = $state<string | null>(null);
  $effect(() => {
    if (!editCanvas || !loaded || deepLinked === editCanvas) return;
    const c = canvases.find((x) => x.uuid === editCanvas);
    if (c) {
      deepLinked = editCanvas;
      openEdit(c);
    }
  });

  function fpsText(c: CanvasInfo): string {
    return c.fpsDen > 0 ? String(Math.round(c.fpsNum / c.fpsDen)) : String(c.fpsNum);
  }

  function encName(list: EncoderType[], id: string): string {
    return list.find((e) => e.id === id)?.name ?? id ?? "—";
  }

  function openAdd() {
    editingUuid = null;
    fName = "";
    fWidth = 1920;
    fHeight = 1080;
    fFps = 60;
    resCustom = false;
    fpsCustom = false;
    fVideoEnc = videoEncoders[0]?.id ?? "";
    fAudioEnc = audioEncoders[0]?.id ?? "";
    formError = null;
    formOpen = true;
  }

  function openEdit(c: CanvasInfo) {
    editingUuid = c.uuid;
    fName = c.name;
    fWidth = c.baseWidth;
    fHeight = c.baseHeight;
    fFps = c.fpsDen > 0 ? Math.round(c.fpsNum / c.fpsDen) : c.fpsNum;
    resCustom = !resPresets.some((p) => p.w === fWidth && p.h === fHeight);
    fpsCustom = !fpsPresets.includes(fFps);
    fVideoEnc = c.videoEncoder || videoEncoders[0]?.id || "";
    fAudioEnc = c.audioEncoder || audioEncoders[0]?.id || "";
    formError = null;
    formOpen = true;
  }

  function closeForm() {
    formOpen = false;
  }

  function positiveInt(n: number, max: number): boolean {
    return Number.isInteger(n) && n > 0 && n <= max;
  }

  const formValid = $derived(
    fName.trim().length > 0 &&
      positiveInt(fWidth, 16384) &&
      positiveInt(fHeight, 16384) &&
      positiveInt(fFps, 1000),
  );

  async function save() {
    if (!formValid || saving) return;
    saving = true;
    formError = null;
    try {
      if (editingUuid) {
        const params: CanvasUpdateParams = {
          uuid: editingUuid,
          name: fName.trim(),
          baseWidth: fWidth,
          baseHeight: fHeight,
          fpsNum: fFps,
          fpsDen: 1,
          videoEncoder: fVideoEnc || undefined,
          audioEncoder: fAudioEnc || undefined,
        };
        await obs.call("canvas.update", params);
      } else {
        const params: CanvasCreateParams = {
          name: fName.trim(),
          baseWidth: fWidth,
          baseHeight: fHeight,
          fpsNum: fFps,
          fpsDen: 1,
          videoEncoder: fVideoEnc || undefined,
          audioEncoder: fAudioEnc || undefined,
        };
        await obs.call("canvas.create", params);
      }
      formOpen = false;
      await loadCanvases();
    } catch (e) {
      formError = (e as Error).message;
    } finally {
      saving = false;
    }
  }

  async function remove(c: CanvasInfo) {
    if (c.isDefault) return;
    try {
      await obs.call("canvas.remove", { uuid: c.uuid });
      if (expandedUuid === c.uuid) expandedUuid = null;
      await loadCanvases();
    } catch (e) {
      error = (e as Error).message;
    }
  }

  function toggleExpand(uuid: string, kind: "video" | "audio") {
    if (expandedUuid === uuid && expandedKind === kind) {
      expandedUuid = null;
    } else {
      expandedUuid = uuid;
      expandedKind = kind;
    }
  }
</script>

<div class="canvases">
  {#if error}<p class="error">{error}</p>{/if}

  {#if !loaded}
    <p class="dim">Loading canvases…</p>
  {:else}
    <ul class="list">
      {#each canvases as c (c.uuid)}
        <li class="row">
          <div class="info">
            <div class="line1">
              <span class="name">{c.name}</span>
              {#if c.isDefault}<span class="badge">Default</span>{/if}
            </div>
            <div class="line2">
              {c.baseWidth} × {c.baseHeight} @ {fpsText(c)} fps
              <span class="sep">·</span>
              {encName(videoEncoders, c.videoEncoder)} / {encName(audioEncoders, c.audioEncoder)}
            </div>
          </div>
          <div class="rowactions">
            <button class="mini" title="Video encoder settings" onclick={() => toggleExpand(c.uuid, "video")}
              >V⚙</button
            >
            <button class="mini" title="Audio encoder settings" onclick={() => toggleExpand(c.uuid, "audio")}
              >A⚙</button
            >
            <button class="mini" title="Edit" onclick={() => openEdit(c)}>✎</button>
            <button class="mini danger" title="Remove" disabled={c.isDefault} onclick={() => void remove(c)}
              >🗑</button
            >
          </div>
        </li>
        {#if expandedUuid === c.uuid}
          <li class="expander">
            <div class="exp-head">{expandedKind === "video" ? "Video" : "Audio"} encoder settings</div>
            {#key c.uuid + ":" + expandedKind}
              <PropertyForm kind="encoder" ref={c.uuid + ":" + expandedKind} />
            {/key}
          </li>
        {/if}
      {/each}
    </ul>

    <div class="addbar">
      <button class="btn" onclick={openAdd}>+ Add Canvas</button>
    </div>
  {/if}

  {#if formOpen}
    <div class="form">
      <h4>{editingUuid ? "Edit Canvas" : "New Canvas"}</h4>

      <div class="field">
        <span class="flabel">Name</span>
        <!-- svelte-ignore a11y_autofocus -->
        <input type="text" bind:value={fName} autofocus placeholder="Canvas name" />
      </div>

      <div class="field">
        <span class="flabel">Resolution</span>
        <div class="presets">
          {#each resPresets as p (p.label)}
            <button
              class="chip"
              class:active={!resCustom && fWidth === p.w && fHeight === p.h}
              onclick={() => {
                fWidth = p.w;
                fHeight = p.h;
                resCustom = false;
              }}>{p.label}</button
            >
          {/each}
          <button class="chip" class:active={resCustom} onclick={() => (resCustom = true)}>Custom</button>
        </div>
        {#if resCustom}
          <div class="wh">
            <input type="number" min="1" max="16384" bind:value={fWidth} aria-label="Width" />
            <span class="x">×</span>
            <input type="number" min="1" max="16384" bind:value={fHeight} aria-label="Height" />
          </div>
        {/if}
      </div>

      <div class="field">
        <span class="flabel">Frame Rate (FPS)</span>
        <div class="presets">
          {#each fpsPresets as f (f)}
            <button
              class="chip"
              class:active={!fpsCustom && fFps === f}
              onclick={() => {
                fFps = f;
                fpsCustom = false;
              }}>{f}</button
            >
          {/each}
          <button class="chip" class:active={fpsCustom} onclick={() => (fpsCustom = true)}>Custom</button>
        </div>
        {#if fpsCustom}
          <div class="wh">
            <input type="number" min="1" max="1000" bind:value={fFps} aria-label="FPS" />
          </div>
        {/if}
      </div>

      <div class="field">
        <span class="flabel">Video Encoder</span>
        <select bind:value={fVideoEnc}>
          {#each videoEncoders as e (e.id)}
            <option value={e.id}>{e.name}</option>
          {/each}
        </select>
      </div>

      <div class="field">
        <span class="flabel">Audio Encoder</span>
        <select bind:value={fAudioEnc}>
          {#each audioEncoders as e (e.id)}
            <option value={e.id}>{e.name}</option>
          {/each}
        </select>
      </div>

      {#if formError}<p class="error">{formError}</p>{/if}

      <div class="actions">
        <button class="btn ghost" onclick={closeForm}>Cancel</button>
        <button class="btn primary" disabled={!formValid || saving} onclick={() => void save()}>
          {saving ? "Saving…" : editingUuid ? "Save" : "Create"}
        </button>
      </div>
    </div>
  {/if}
</div>

<style>
  .canvases {
    padding: 8px 0 4px;
  }
  .list {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    padding: 8px 10px;
    border: 1px solid var(--border);
    border-radius: 8px;
    background: var(--bg-sunken);
  }
  .info {
    min-width: 0;
  }
  .line1 {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .name {
    font-size: 13px;
    color: var(--text);
    font-weight: 500;
  }
  .badge {
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--color-accent-contrast);
    background: var(--accent);
    border-radius: 999px;
    padding: 1px 7px;
  }
  .line2 {
    font-size: 11px;
    color: var(--text-dim);
    margin-top: 2px;
  }
  .sep {
    margin: 0 4px;
  }
  .rowactions {
    display: flex;
    gap: 4px;
    flex-shrink: 0;
  }
  .mini {
    background: none;
    border: 1px solid var(--border);
    border-radius: 6px;
    color: var(--text-soft);
    cursor: pointer;
    font: inherit;
    font-size: 12px;
    padding: 4px 7px;
    line-height: 1;
  }
  .mini:hover:not(:disabled) {
    color: var(--text);
    background: var(--bg-raised);
  }
  .mini.danger:hover:not(:disabled) {
    color: var(--off, #d65a5a);
    border-color: var(--off, #d65a5a);
  }
  .mini:disabled {
    opacity: 0.35;
    cursor: default;
  }
  .expander {
    border: 1px solid var(--border);
    border-top: none;
    border-radius: 0 0 8px 8px;
    background: var(--bg-raised);
    padding: 12px 12px 14px;
    margin: -4px 0 2px;
  }
  .exp-head {
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--text-dim);
    margin-bottom: 10px;
  }
  .addbar {
    margin-top: 12px;
  }
  .form {
    margin-top: 14px;
    padding: 14px;
    border: 1px solid var(--border);
    border-radius: 10px;
    background: var(--bg-raised);
  }
  .form h4 {
    margin: 0 0 12px;
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--text-dim);
  }
  .field {
    margin-bottom: 12px;
  }
  .flabel {
    display: block;
    font-size: 12px;
    color: var(--text-soft);
    margin-bottom: 6px;
  }
  .presets {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    margin-bottom: 6px;
  }
  .chip {
    border: 1px solid var(--border);
    background: var(--bg-sunken);
    color: var(--text-soft);
    border-radius: 999px;
    padding: 4px 11px;
    font: inherit;
    font-size: 12px;
    cursor: pointer;
  }
  .chip:hover {
    color: var(--text);
  }
  .chip.active {
    background: var(--accent);
    border-color: var(--accent);
    color: var(--color-accent-contrast);
  }
  .wh {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .x {
    color: var(--text-dim);
  }
  input,
  select {
    background: var(--bg-sunken);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 7px 10px;
    color: var(--text);
    font: inherit;
  }
  input[type="number"] {
    width: 96px;
  }
  input[type="text"],
  select {
    width: 100%;
  }
  input[type="text"] {
    max-width: 340px;
  }
  select {
    max-width: 420px;
  }
  input:focus,
  select:focus {
    outline: none;
    border-color: var(--accent);
  }
  .actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
    margin-top: 4px;
  }
  .btn {
    border-radius: 6px;
    padding: 7px 14px;
    font: inherit;
    cursor: pointer;
    border: 1px solid var(--border);
    background: var(--bg-sunken);
    color: var(--text-soft);
  }
  .btn:hover:not(:disabled) {
    color: var(--text);
  }
  .btn.primary {
    background: var(--accent);
    border-color: var(--accent);
    color: var(--color-accent-contrast);
  }
  .btn.primary:disabled {
    opacity: 0.45;
    cursor: default;
  }
  .btn.ghost {
    background: none;
  }
  .dim {
    color: var(--text-dim);
    margin: 0;
  }
  .error {
    color: var(--off, #d65a5a);
    margin: 0 0 8px;
    font-size: 12px;
  }
</style>
