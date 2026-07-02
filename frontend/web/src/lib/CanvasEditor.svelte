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
    /** Hosted inline in a master-detail pane (no modal): hide the inert Cancel. */
    embedded?: boolean;
  }
  let { canvas, videoEncoders, audioEncoders, onClose, onSaved, embedded = false }: Props = $props();

  type Orient = "h" | "v" | "custom";
  type Tab = "video" | "audio" | "advanced";

  interface ResPreset {
    label: string;
    w: number;
    h: number;
  }
  // Landscape / portrait resolution presets, both descending. The orientation
  // segment (Horizontal | Vertical | Custom) picks which list the chips show.
  const H_PRESETS: ResPreset[] = [
    { label: "3840 × 2160", w: 3840, h: 2160 },
    { label: "2560 × 1440", w: 2560, h: 1440 },
    { label: "1920 × 1080", w: 1920, h: 1080 },
    { label: "1600 × 900", w: 1600, h: 900 },
    { label: "1280 × 720", w: 1280, h: 720 },
  ];
  const V_PRESETS: ResPreset[] = [
    { label: "2160 × 3840", w: 2160, h: 3840 },
    { label: "1440 × 2560", w: 1440, h: 2560 },
    { label: "1080 × 1920", w: 1080, h: 1920 },
    { label: "720 × 1280", w: 720, h: 1280 },
  ];
  const ORIENTS: { value: Orient; label: string }[] = [
    { value: "h", label: "Horizontal" },
    { value: "v", label: "Vertical" },
    { value: "custom", label: "Custom" },
  ];
  // Integer frame-rate presets; fractional rates (59.94 = 60000/1001) live under Custom.
  const fpsPresets: number[] = [24, 30, 48, 60, 120];
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

  // Which orientation segment a resolution belongs to: whichever preset list holds
  // it, else Custom (a value matching no preset lands on Custom by definition).
  function orientFor(w: number, h: number): Orient {
    if (H_PRESETS.some((p) => p.w === w && p.h === h)) return "h";
    if (V_PRESETS.some((p) => p.w === w && p.h === h)) return "v";
    return "custom";
  }
  function presetsFor(o: Orient): ResPreset[] {
    return o === "v" ? V_PRESETS : H_PRESETS;
  }

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
  // svelte-ignore state_referenced_locally
  let resOrient = $state<Orient>(orientFor(fWidth, fHeight));
  // svelte-ignore state_referenced_locally
  let outOrient = $state<Orient>(orientFor(fOutWidth, fOutHeight));
  // Integer preset selected iff whole-number fps that matches the preset list.
  let fpsCustom = $state(c ? !(c.fpsDen === 1 && fpsPresets.includes(c.fpsNum)) : false);
  // svelte-ignore state_referenced_locally
  let fVideoEnc = $state(c?.videoEncoder || videoEncoders[0]?.id || "");
  // svelte-ignore state_referenced_locally
  let fAudioEnc = $state(c?.audioEncoder || audioEncoders[0]?.id || "");
  let fColorFormat = $state(c?.color.format ?? "NV12");
  let fColorSpace = $state(c?.color.space ?? "709");
  let fColorRange = $state(c?.color.range ?? "Partial");
  let fColorUseDefault = $state(c?.color.useDefault ?? false);
  // Inheritance flags (non-default canvases). When on, the backend swaps in the
  // Default canvas's resolution/fps or encoders and ignores the values we still send.
  let fUseDefaultRes = $state(c?.useDefaultResolution ?? false);
  let fVideoUseDefault = $state(c?.videoUseDefault ?? false);
  let fAudioUseDefault = $state(c?.audioUseDefault ?? false);

  let activeTab = $state<Tab>("video");
  let saving = $state(false);
  let formError = $state<string | null>(null);

  function positiveInt(n: number, max: number): boolean {
    return Number.isInteger(n) && n > 0 && n <= max;
  }

  // The Default canvas has nothing to inherit, so it shows no "use default" toggles
  // and its name is fixed.
  const editingDefaultCanvas = $derived(canvas?.isDefault ?? false);

  const formValid = $derived(
    fName.trim().length > 0 &&
      (fUseDefaultRes ||
        (positiveInt(fWidth, 16384) &&
          positiveInt(fHeight, 16384) &&
          positiveInt(fFpsNum, 144000) &&
          fFpsDen > 0 &&
          (fOutSameAsBase || (positiveInt(fOutWidth, 16384) && positiveInt(fOutHeight, 16384))))),
  );

  const tabs: { id: Tab; label: string }[] = [
    { id: "video", label: "Video" },
    { id: "audio", label: "Audio" },
    { id: "advanced", label: "Advanced" },
  ];

  async function save() {
    if (!formValid || saving) return;
    saving = true;
    formError = null;
    // The flags decide inheritance; the underlying values ride along (ignored by the
    // backend while a flag inherits, kept fresh so toggling back is lossless).
    const shared = {
      baseWidth: fWidth,
      baseHeight: fHeight,
      outputWidth: fOutSameAsBase ? 0 : fOutWidth,
      outputHeight: fOutSameAsBase ? 0 : fOutHeight,
      fpsNum: fFpsNum,
      fpsDen: fFpsDen,
      scaleType: fScaleType,
      useDefaultResolution: fUseDefaultRes,
      videoEncoder: fVideoEnc || undefined,
      audioEncoder: fAudioEnc || undefined,
      videoUseDefault: fVideoUseDefault,
      audioUseDefault: fAudioUseDefault,
      color: { format: fColorFormat, space: fColorSpace, range: fColorRange, useDefault: fColorUseDefault },
    };
    try {
      if (canvas) {
        const params: CanvasUpdateParams = { uuid: canvas.uuid, name: fName.trim(), ...shared };
        await obs.call("canvas.update", params);
      } else {
        const params: CanvasCreateParams = { name: fName.trim(), ...shared };
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

{#snippet toggle(checked: boolean, disabled: boolean, label: string, onchange: (v: boolean) => void)}
  <label class="switch-row" class:dis={disabled}>
    <span class="switch">
      <input type="checkbox" {checked} {disabled} onchange={(e) => onchange(e.currentTarget.checked)} />
      <span class="track"><span class="thumb2"></span></span>
    </span>
    <span class="switch-label">{label}</span>
  </label>
{/snippet}

{#snippet segmented(value: Orient, disabled: boolean, onpick: (o: Orient) => void)}
  <div class="seg" class:dis={disabled} role="tablist" aria-label="Orientation">
    {#each ORIENTS as o (o.value)}
      <button type="button" class="seg-btn" class:on={value === o.value} {disabled} onclick={() => onpick(o.value)}>
        {o.label}
      </button>
    {/each}
  </div>
{/snippet}

{#snippet presetChips(list: ResPreset[], w: number, h: number, disabled: boolean, onpick: (w: number, h: number) => void)}
  <div class="presets">
    {#each list as p (p.label)}
      <button
        type="button"
        class="chip"
        class:active={w === p.w && h === p.h}
        {disabled}
        onclick={() => onpick(p.w, p.h)}>{p.label}</button
      >
    {/each}
  </div>
{/snippet}

<div class="form">
  <h4>{canvas ? "Edit Canvas" : "New Canvas"}</h4>

  <div class="field">
    <span class="flabel">Name</span>
    <!-- svelte-ignore a11y_autofocus -->
    <input type="text" bind:value={fName} autofocus placeholder="Canvas name" disabled={editingDefaultCanvas} />
  </div>

  {#if !editingDefaultCanvas}
    {@render toggle(fUseDefaultRes, false, "Use Default resolution", (v) => (fUseDefaultRes = v))}
  {/if}

  <div class="field">
    <span class="flabel">Resolution</span>
    {@render segmented(resOrient, fUseDefaultRes, (o) => (resOrient = o))}
    {#if resOrient === "custom"}
      <div class="wh">
        <input
          type="number"
          min="1"
          max="16384"
          bind:value={fWidth}
          disabled={fUseDefaultRes}
          aria-label="Width"
        />
        <span class="x">×</span>
        <input
          type="number"
          min="1"
          max="16384"
          bind:value={fHeight}
          disabled={fUseDefaultRes}
          aria-label="Height"
        />
      </div>
    {:else}
      {@render presetChips(presetsFor(resOrient), fWidth, fHeight, fUseDefaultRes, (w, h) => {
        fWidth = w;
        fHeight = h;
      })}
    {/if}
  </div>

  <div class="field">
    <span class="flabel">Output Resolution (scaled)</span>
    {@render toggle(fOutSameAsBase, fUseDefaultRes, "Same as base", (v) => (fOutSameAsBase = v))}
    {#if !fOutSameAsBase}
      {@render segmented(outOrient, fUseDefaultRes, (o) => (outOrient = o))}
      {#if outOrient === "custom"}
        <div class="wh">
          <input
            type="number"
            min="1"
            max="16384"
            bind:value={fOutWidth}
            disabled={fUseDefaultRes}
            aria-label="Output width"
          />
          <span class="x">×</span>
          <input
            type="number"
            min="1"
            max="16384"
            bind:value={fOutHeight}
            disabled={fUseDefaultRes}
            aria-label="Output height"
          />
        </div>
      {:else}
        {@render presetChips(presetsFor(outOrient), fOutWidth, fOutHeight, fUseDefaultRes, (w, h) => {
          fOutWidth = w;
          fOutHeight = h;
        })}
      {/if}
    {/if}
  </div>

  <div class="field">
    <span class="flabel">Downscale Filter</span>
    <select bind:value={fScaleType} disabled={fUseDefaultRes}>
      {#each scaleTypes as s (s.value)}
        <option value={s.value}>{s.label}</option>
      {/each}
    </select>
  </div>

  <div class="field">
    <span class="flabel">Frame Rate (FPS)</span>
    <div class="presets">
      {#each fpsPresets as p (p)}
        <button
          type="button"
          class="chip"
          class:active={!fpsCustom && fFpsNum === p && fFpsDen === 1}
          disabled={fUseDefaultRes}
          onclick={() => {
            fFpsNum = p;
            fFpsDen = 1;
            fpsCustom = false;
          }}>{p}</button
        >
      {/each}
      <button
        type="button"
        class="chip"
        class:active={fpsCustom}
        disabled={fUseDefaultRes}
        onclick={() => (fpsCustom = true)}>Custom</button
      >
    </div>
    {#if fpsCustom}
      <div class="wh">
        <input type="number" min="1" max="144000" bind:value={fFpsNum} disabled={fUseDefaultRes} aria-label="FPS numerator" />
        <span class="x">/</span>
        <input type="number" min="1" max="1001" bind:value={fFpsDen} disabled={fUseDefaultRes} aria-label="FPS denominator" />
      </div>
    {/if}
  </div>

  <div class="tabs" role="tablist" aria-label="Encoder settings">
    {#each tabs as t (t.id)}
      <button
        type="button"
        class="tab"
        class:on={activeTab === t.id}
        role="tab"
        aria-selected={activeTab === t.id}
        onclick={() => (activeTab = t.id)}>{t.label}</button
      >
    {/each}
  </div>

  <div class="tabpanel">
    {#if activeTab === "video"}
      {#if !editingDefaultCanvas}
        {@render toggle(fVideoUseDefault, false, "Use Default video", (v) => (fVideoUseDefault = v))}
      {/if}
      <div class="field">
        <span class="flabel">Video Encoder</span>
        <select bind:value={fVideoEnc} disabled={fVideoUseDefault}>
          {#each videoEncoders as e (e.id)}
            <option value={e.id}>{e.name}</option>
          {/each}
        </select>
      </div>
    {:else if activeTab === "audio"}
      {#if !editingDefaultCanvas}
        {@render toggle(fAudioUseDefault, false, "Use Default audio", (v) => (fAudioUseDefault = v))}
      {/if}
      <div class="field">
        <span class="flabel">Audio Encoder</span>
        <select bind:value={fAudioEnc} disabled={fAudioUseDefault}>
          {#each audioEncoders as e (e.id)}
            <option value={e.id}>{e.name}</option>
          {/each}
        </select>
      </div>
    {:else}
      {#if !editingDefaultCanvas}
        {@render toggle(fColorUseDefault, false, "Use Default canvas color settings", (v) => (fColorUseDefault = v))}
      {/if}
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
    {/if}
  </div>

  {#if formError}<p class="error">{formError}</p>{/if}

  <div class="actions">
    {#if !embedded}
      <button class="btn ghost" onclick={onClose}>Cancel</button>
    {/if}
    <button class="btn primary" disabled={!formValid || saving} onclick={() => void save()}>
      {saving ? "Saving…" : canvas ? "Save" : "Create"}
    </button>
  </div>
</div>

<style>
  .form {
    margin-top: 14px;
    padding: 14px;
    border: 1px solid var(--color-border);
    background: var(--color-surface);
  }
  .form h4 {
    margin: 0 0 12px;
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--color-muted);
  }
  .field {
    margin-bottom: 12px;
  }
  .flabel {
    display: block;
    font-size: 12px;
    color: var(--color-text);
    margin-bottom: 6px;
  }
  .presets {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    margin-bottom: 6px;
  }
  .chip {
    border: 1px solid var(--color-border);
    background: var(--color-base);
    color: var(--color-text);
    padding: 4px 11px;
    font: inherit;
    font-size: 12px;
    cursor: pointer;
  }
  .chip:hover:not(:disabled) {
    color: var(--color-text);
  }
  .chip.active {
    background: var(--color-accent);
    border-color: var(--color-accent);
    color: var(--color-accent-contrast);
  }
  .chip:disabled {
    opacity: 0.4;
    cursor: default;
  }
  .wh {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .x {
    color: var(--color-muted);
  }
  input,
  select {
    background: var(--color-base);
    border: 1px solid var(--color-border);
    padding: 7px 10px;
    color: var(--color-text);
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
    border-color: var(--color-accent);
  }
  input:disabled,
  select:disabled {
    opacity: 0.4;
    cursor: default;
  }

  /* ---- orientation segmented control (new: --color-* tokens) -------------- */
  .seg {
    display: inline-flex;
    margin-bottom: 6px;
    border: var(--border-weight) solid var(--color-border);
  }
  .seg.dis {
    opacity: 0.4;
  }
  .seg-btn {
    border: 0;
    border-right: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
    color: var(--color-muted);
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.04em;
    text-transform: uppercase;
    padding: 5px 12px;
    cursor: pointer;
  }
  .seg-btn:last-child {
    border-right: 0;
  }
  .seg-btn:hover:not(:disabled) {
    color: var(--color-text);
  }
  .seg-btn.on {
    background: var(--color-accent);
    color: var(--color-accent-ink);
  }
  .seg-btn:disabled {
    cursor: default;
  }

  /* ---- tab strip (new) --------------------------------------------------- */
  .tabs {
    display: flex;
    gap: 0;
    margin: 6px 0 14px;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .tab {
    border: 0;
    border-bottom: 2px solid transparent;
    background: none;
    color: var(--color-muted);
    font-family: var(--font-ui);
    font-size: 12px;
    font-weight: 600;
    padding: 8px 14px;
    margin-bottom: -1px;
    cursor: pointer;
  }
  .tab:hover {
    color: var(--color-text);
  }
  .tab.on {
    color: var(--color-text);
    border-bottom-color: var(--color-accent);
  }
  .tabpanel {
    margin-bottom: 4px;
  }

  /* ---- use-default toggle switch (new: mirrors the app switch) ----------- */
  .switch-row {
    display: flex;
    align-items: center;
    gap: 10px;
    margin-bottom: 12px;
    cursor: pointer;
  }
  .switch-row.dis {
    opacity: 0.5;
    cursor: default;
  }
  .switch-label {
    font-size: 12px;
    color: var(--color-text);
  }
  .switch {
    flex: 0 0 auto;
    display: inline-flex;
    align-items: center;
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
  .switch input:checked + .track {
    background: color-mix(in srgb, var(--color-accent) 30%, transparent);
    border-color: var(--color-accent);
  }
  .switch input:checked + .track .thumb2 {
    left: calc(100% - 12px - 3px);
    background: var(--color-accent);
  }
  .switch input:focus-visible + .track {
    outline: 1px solid var(--color-accent);
    outline-offset: 1px;
  }

  .actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
    margin-top: 4px;
  }
  .btn {
    padding: 7px 14px;
    font: inherit;
    cursor: pointer;
    border: 1px solid var(--color-border);
    background: var(--color-base);
    color: var(--color-text);
  }
  .btn:hover:not(:disabled) {
    color: var(--color-text);
  }
  .btn.primary {
    background: var(--color-accent);
    border-color: var(--color-accent);
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
    color: var(--color-live);
    margin: 0 0 8px;
    font-size: 12px;
  }
</style>
