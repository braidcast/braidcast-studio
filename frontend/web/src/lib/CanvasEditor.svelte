<script lang="ts">
  import {
    obs,
    type CanvasInfo,
    type EncoderType,
    type CanvasCreateParams,
    type CanvasUpdateParams,
  } from "./bridge";

  interface Props {
    /** The canvas being edited, or null to create a new one. */
    canvas: CanvasInfo | null;
    videoEncoders: EncoderType[];
    audioEncoders: EncoderType[];
    /** Close without persisting (Cancel). */
    onClose: () => void;
    /** Called after a successful create/update so the parent can refresh + close. */
    onSaved: () => void;
  }
  let { canvas, videoEncoders, audioEncoders, onClose, onSaved }: Props = $props();

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
  // Color format/space/range tokens (option VALUES are the exact backend tokens).
  const colorFormats: { label: string; value: string }[] = [
    { label: "NV12 (8-bit)", value: "NV12" },
    { label: "I420 (8-bit)", value: "I420" },
    { label: "I444 (8-bit)", value: "I444" },
    { label: "I010 (10-bit)", value: "I010" },
    { label: "P010 (10-bit)", value: "P010" },
    { label: "P216 (16-bit)", value: "P216" },
    { label: "P416 (16-bit)", value: "P416" },
    { label: "RGB (8-bit)", value: "RGB" },
  ];
  const colorSpaces: { label: string; value: string }[] = [
    { label: "601 (SDR)", value: "601" },
    { label: "709 (SDR)", value: "709" },
    { label: "2100PQ (HDR)", value: "2100PQ" },
    { label: "2100HLG (HDR)", value: "2100HLG" },
    { label: "sRGB", value: "sRGB" },
  ];
  const colorRanges: { label: string; value: string }[] = [
    { label: "Partial", value: "Partial" },
    { label: "Full", value: "Full" },
  ];

  // Initial form state, seeded from the canvas prop (null => add defaults). The
  // parent keys this component by canvas uuid, so it remounts (re-seeds) whenever
  // a different canvas is opened — matching the old imperative openEdit/openAdd.
  // svelte-ignore state_referenced_locally
  const c = canvas;
  const sameOut = c ? c.outputWidth === c.baseWidth && c.outputHeight === c.baseHeight : true;

  let fName = $state(c?.name ?? "");
  let fWidth = $state(c?.baseWidth ?? 1920);
  let fHeight = $state(c?.baseHeight ?? 1080);
  let fFpsNum = $state(c?.fpsNum ?? 60);
  let fFpsDen = $state(c?.fpsDen ?? 1);
  let fOutSameAsBase = $state(sameOut);
  let fOutWidth = $state(c?.outputWidth ?? 1920);
  let fOutHeight = $state(c?.outputHeight ?? 1080);
  let fScaleType = $state(c?.scaleType || "bicubic");
  let resCustom = $state(c ? !resPresets.some((p) => p.w === c.baseWidth && p.h === c.baseHeight) : false);
  let fpsCustom = $state(c ? !fpsPresets.some((p) => p.num === c.fpsNum && p.den === c.fpsDen) : false);
  let outCustom = $state(
    c ? !sameOut && !resPresets.some((p) => p.w === c.outputWidth && p.h === c.outputHeight) : false,
  );
  // svelte-ignore state_referenced_locally
  let fVideoEnc = $state(c?.videoEncoder || videoEncoders[0]?.id || "");
  // svelte-ignore state_referenced_locally
  let fAudioEnc = $state(c?.audioEncoder || audioEncoders[0]?.id || "");
  let fColorFormat = $state(c?.color.format ?? "NV12");
  let fColorSpace = $state(c?.color.space ?? "709");
  let fColorRange = $state(c?.color.range ?? "Partial");
  let fColorUseDefault = $state(c?.color.useDefault ?? false);
  let saving = $state(false);
  let formError = $state<string | null>(null);

  function positiveInt(n: number, max: number): boolean {
    return Number.isInteger(n) && n > 0 && n <= max;
  }

  // The Default canvas has nothing to inherit, so its "use default color" toggle is
  // hidden.
  const editingDefaultCanvas = $derived(canvas?.isDefault ?? false);

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
      if (canvas) {
        const params: CanvasUpdateParams = {
          uuid: canvas.uuid,
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
          color: { format: fColorFormat, space: fColorSpace, range: fColorRange, useDefault: fColorUseDefault },
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
          color: { format: fColorFormat, space: fColorSpace, range: fColorRange, useDefault: fColorUseDefault },
        };
        await obs.call("canvas.create", params);
      }
      onSaved();
    } catch (e) {
      formError = (e as Error).message;
    } finally {
      saving = false;
    }
  }
</script>

<div class="form">
  <h4>{canvas ? "Edit Canvas" : "New Canvas"}</h4>

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

  <div class="field">
    <span class="flabel">Advanced (Color)</span>
    {#if !editingDefaultCanvas}
      <label class="colorcheck">
        <input type="checkbox" bind:checked={fColorUseDefault} />
        Use Default canvas color settings
      </label>
    {/if}
  </div>

  <div class="field">
    <span class="flabel">Color Format</span>
    <select bind:value={fColorFormat} disabled={fColorUseDefault}>
      {#each colorFormats as f (f.value)}
        <option value={f.value}>{f.label}</option>
      {/each}
    </select>
  </div>

  <div class="field">
    <span class="flabel">Color Space</span>
    <select bind:value={fColorSpace} disabled={fColorUseDefault}>
      {#each colorSpaces as cs (cs.value)}
        <option value={cs.value}>{cs.label}</option>
      {/each}
    </select>
  </div>

  <div class="field">
    <span class="flabel">Color Range</span>
    <select bind:value={fColorRange} disabled={fColorUseDefault}>
      {#each colorRanges as r (r.value)}
        <option value={r.value}>{r.label}</option>
      {/each}
    </select>
  </div>

  {#if formError}<p class="error">{formError}</p>{/if}

  <div class="actions">
    <button class="btn ghost" onclick={onClose}>Cancel</button>
    <button class="btn primary" disabled={!formValid || saving} onclick={() => void save()}>
      {saving ? "Saving…" : canvas ? "Save" : "Create"}
    </button>
  </div>
</div>

<style>
  .form {
    margin-top: 14px;
    padding: 14px;
    border: 1px solid var(--border);
    border-radius: 0;
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
    border-radius: 0;
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
  .colorcheck {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 12px;
    color: var(--text-soft);
    cursor: pointer;
  }
  .colorcheck input[type="checkbox"] {
    width: auto;
    background: none;
    border: 0;
    border-radius: 0;
    padding: 0;
    cursor: pointer;
  }
  .x {
    color: var(--text-dim);
  }
  input,
  select {
    background: var(--bg-sunken);
    border: 1px solid var(--border);
    border-radius: 0;
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
    border-radius: 0;
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
  .error {
    color: var(--off, #d65a5a);
    margin: 0 0 8px;
    font-size: 12px;
  }
</style>
