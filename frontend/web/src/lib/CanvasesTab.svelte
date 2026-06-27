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
  const fpsPresets: { label: string; num: number; den: number }[] = [
    { label: "24", num: 24, den: 1 },
    { label: "30", num: 30, den: 1 },
    { label: "48", num: 48, den: 1 },
    { label: "60", num: 60, den: 1 },
    { label: "23.976", num: 24000, den: 1001 },
    { label: "29.97", num: 30000, den: 1001 },
    { label: "59.94", num: 60000, den: 1001 },
  ];
  const scaleTypes: { label: string; value: string }[] = [
    { label: "Bilinear", value: "bilinear" },
    { label: "Bicubic", value: "bicubic" },
    { label: "Lanczos", value: "lanczos" },
    { label: "Area", value: "area" },
  ];

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
  let fFpsNum = $state(60);
  let fFpsDen = $state(1);
  let fOutSameAsBase = $state(true);
  let fOutWidth = $state(1920);
  let fOutHeight = $state(1080);
  let fScaleType = $state("bicubic");
  let resCustom = $state(false);
  let fpsCustom = $state(false);
  let outCustom = $state(false);
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
    if (!(c.fpsDen > 0)) return String(c.fpsNum);
    return c.fpsDen > 1 ? (c.fpsNum / c.fpsDen).toFixed(2) : String(c.fpsNum);
  }

  function encName(list: EncoderType[], id: string): string {
    return list.find((e) => e.id === id)?.name ?? id ?? "—";
  }

  function gcd(a: number, b: number): number {
    return b === 0 ? a : gcd(b, a % b);
  }
  function ratioLabel(w: number, h: number): string {
    if (!(w > 0) || !(h > 0)) {
      return "";
    }
    const g = gcd(w, h) || 1;
    return `${w / g}:${h / g}`;
  }

  function openAdd() {
    editingUuid = null;
    fName = "";
    fWidth = 1920;
    fHeight = 1080;
    fFpsNum = 60;
    fFpsDen = 1;
    fOutSameAsBase = true;
    fOutWidth = 1920;
    fOutHeight = 1080;
    fScaleType = "bicubic";
    resCustom = false;
    fpsCustom = false;
    outCustom = false;
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
    fFpsNum = c.fpsNum;
    fFpsDen = c.fpsDen;
    fOutSameAsBase = c.outputWidth === c.baseWidth && c.outputHeight === c.baseHeight;
    fOutWidth = c.outputWidth;
    fOutHeight = c.outputHeight;
    fScaleType = c.scaleType || "bicubic";
    resCustom = !resPresets.some((p) => p.w === fWidth && p.h === fHeight);
    fpsCustom = !fpsPresets.some((p) => p.num === c.fpsNum && p.den === c.fpsDen);
    outCustom = !fOutSameAsBase && !resPresets.some((p) => p.w === c.outputWidth && p.h === c.outputHeight);
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
      positiveInt(fFpsNum, 144000) &&
      fFpsDen > 0 &&
      (fOutSameAsBase || (positiveInt(fOutWidth, 16384) && positiveInt(fOutHeight, 16384))),
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
          outputWidth: fOutSameAsBase ? 0 : fOutWidth,
          outputHeight: fOutSameAsBase ? 0 : fOutHeight,
          fpsNum: fFpsNum,
          fpsDen: fFpsDen,
          scaleType: fScaleType,
          videoEncoder: fVideoEnc || undefined,
          audioEncoder: fAudioEnc || undefined,
        };
        await obs.call("canvas.update", params);
      } else {
        const params: CanvasCreateParams = {
          name: fName.trim(),
          baseWidth: fWidth,
          baseHeight: fHeight,
          outputWidth: fOutSameAsBase ? 0 : fOutWidth,
          outputHeight: fOutSameAsBase ? 0 : fOutHeight,
          fpsNum: fFpsNum,
          fpsDen: fFpsDen,
          scaleType: fScaleType,
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

  // The expander renders once below the grid (not as a grid cell), so resolve the
  // open canvas from its uuid. Clears itself if that canvas is removed.
  const expandedCanvas = $derived(expandedUuid ? (canvases.find((c) => c.uuid === expandedUuid) ?? null) : null);
</script>

<div class="canvases">
  {#if error}<p class="error">{error}</p>{/if}

  {#if !loaded}
    <p class="dim">Loading canvases…</p>
  {:else}
    <ul class="grid">
      {#each canvases as c (c.uuid)}
        {@const portrait = c.baseHeight > c.baseWidth}
        <li class="tile">
          <button class="preview-box" onclick={() => openEdit(c)} title="Edit “{c.name}”">
            <div class="preview-square">
              <div class="preview-frame" class:tall={portrait} style:aspect-ratio="{c.baseWidth} / {c.baseHeight}">
                <span class="res-chip">{c.baseWidth} × {c.baseHeight}</span>
              </div>
            </div>
          </button>
          <div class="tile-foot">
            <div class="info">
              <div class="line1">
                <span class="name">{c.name}</span>
                {#if c.isDefault}<span class="badge">Default</span>{/if}
                <span class="ratio">{ratioLabel(c.baseWidth, c.baseHeight)}</span>
              </div>
              <div class="line2">
                {fpsText(c)} fps · {encName(videoEncoders, c.videoEncoder)} / {encName(audioEncoders, c.audioEncoder)}
              </div>
            </div>
            <div class="rowactions">
              <button class="mini" title="Video encoder settings" onclick={() => toggleExpand(c.uuid, "video")}>V⚙</button>
              <button class="mini" title="Audio encoder settings" onclick={() => toggleExpand(c.uuid, "audio")}>A⚙</button>
              <button class="mini" title="Edit" onclick={() => openEdit(c)}>✎</button>
              <button class="mini danger" title="Remove" disabled={c.isDefault} onclick={() => void remove(c)}>🗑</button>
            </div>
          </div>
        </li>
      {/each}
    </ul>

    {#if expandedCanvas}
      <div class="expander">
        <div class="exp-head">
          {expandedCanvas.name} · {expandedKind === "video" ? "Video" : "Audio"} encoder settings
        </div>
        {#key expandedCanvas.uuid + ":" + expandedKind}
          <PropertyForm kind="encoder" ref={expandedCanvas.uuid + ":" + expandedKind} />
        {/key}
      </div>
    {/if}

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
        <span class="flabel">Output Resolution (scaled)</span>
        <div class="presets">
          <button
            class="chip"
            class:active={fOutSameAsBase}
            onclick={() => {
              fOutSameAsBase = true;
              outCustom = false;
            }}>Same as base</button
          >
          {#each resPresets as p (p.label)}
            <button
              class="chip"
              class:active={!fOutSameAsBase && !outCustom && fOutWidth === p.w && fOutHeight === p.h}
              onclick={() => {
                fOutSameAsBase = false;
                fOutWidth = p.w;
                fOutHeight = p.h;
                outCustom = false;
              }}>{p.label}</button
            >
          {/each}
          <button
            class="chip"
            class:active={!fOutSameAsBase && outCustom}
            onclick={() => {
              fOutSameAsBase = false;
              outCustom = true;
            }}>Custom</button
          >
        </div>
        {#if !fOutSameAsBase && outCustom}
          <div class="wh">
            <input type="number" min="1" max="16384" bind:value={fOutWidth} aria-label="Output width" />
            <span class="x">×</span>
            <input type="number" min="1" max="16384" bind:value={fOutHeight} aria-label="Output height" />
          </div>
        {/if}
      </div>

      <div class="field">
        <span class="flabel">Downscale Filter</span>
        <select bind:value={fScaleType}>
          {#each scaleTypes as s (s.value)}
            <option value={s.value}>{s.label}</option>
          {/each}
        </select>
      </div>

      <div class="field">
        <span class="flabel">Frame Rate (FPS)</span>
        <div class="presets">
          {#each fpsPresets as p (p.label)}
            <button
              class="chip"
              class:active={!fpsCustom && fFpsNum === p.num && fFpsDen === p.den}
              onclick={() => {
                fFpsNum = p.num;
                fFpsDen = p.den;
                fpsCustom = false;
              }}>{p.label}</button
            >
          {/each}
          <button
            class="chip"
            class:active={fpsCustom}
            onclick={() => {
              // Collapse a fractional preset (num/den) to a sane whole-number seed
              // so Custom doesn't pre-fill an absurd value like 30000.
              fFpsNum = fFpsDen > 1 ? Math.round(fFpsNum / fFpsDen) : fFpsNum;
              fpsCustom = true;
              fFpsDen = 1;
            }}>Custom</button
          >
        </div>
        {#if fpsCustom}
          <div class="wh">
            <input type="number" min="1" max="144000" bind:value={fFpsNum} aria-label="FPS" />
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
  .grid {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
    gap: 14px;
  }
  .tile {
    display: flex;
    flex-direction: column;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .preview-box {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 100%;
    height: 168px;
    padding: 18px;
    background: var(--color-base);
    border: 0;
    border-bottom: var(--border-weight) solid var(--color-border);
    cursor: pointer;
  }
  .preview-box:hover {
    background: color-mix(in srgb, var(--color-text) 3%, var(--color-base));
  }
  .preview-square {
    width: 132px;
    height: 132px;
    display: flex;
    align-items: center;
    justify-content: center;
  }
  .preview-frame {
    position: relative;
    width: 100%;
    height: auto;
    max-height: 100%;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
    box-shadow: 0 2px 10px rgba(0, 0, 0, 0.35);
  }
  .preview-frame.tall {
    width: auto;
    height: 100%;
    max-width: 100%;
  }
  .res-chip {
    position: absolute;
    top: 5px;
    left: 5px;
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.04em;
    color: var(--color-dim);
    background: color-mix(in srgb, var(--color-base) 70%, transparent);
    padding: 1px 4px;
    white-space: nowrap;
  }
  .tile-foot {
    display: flex;
    align-items: flex-start;
    gap: 10px;
    padding: 12px 12px 10px;
  }
  .info {
    min-width: 0;
    flex: 1;
  }
  .line1 {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .name {
    font-size: 13px;
    color: var(--color-text);
    font-weight: 500;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .badge {
    flex: 0 0 auto;
    font-size: 9px;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--color-accent-ink);
    background: var(--color-accent);
    padding: 1px 6px;
  }
  .ratio {
    margin-left: auto;
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .line2 {
    font-size: 11px;
    color: var(--color-muted);
    margin-top: 3px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .rowactions {
    display: flex;
    gap: 4px;
    justify-content: flex-end;
    flex: 0 0 auto;
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
    margin-top: 12px;
    border: 1px solid var(--border);
    background: var(--bg-raised);
    padding: 12px 12px 14px;
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
