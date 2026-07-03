<script lang="ts">
  import type { CanvasForm } from "./canvasForm";
  import { FPS_PRESETS } from "./canvasForm";
  import UseDefaultStrip from "./UseDefaultStrip.svelte";
  import Icon from "../dock/Icon.svelte";

  interface Props {
    form: CanvasForm;
    isLive: boolean;
    isDefault: boolean;
    commit: () => void;
  }
  let { form, isLive, isDefault, commit }: Props = $props();

  type Orient = "h" | "v" | "custom";
  interface ResPreset {
    label: string;
    w: number;
    h: number;
  }

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
  const scaleTypes = [
    { label: "Bilinear", value: "bilinear" },
    { label: "Bicubic", value: "bicubic" },
    { label: "Lanczos", value: "lanczos" },
    { label: "Area", value: "area" },
  ];

  function orientFor(w: number, h: number): Orient {
    if (H_PRESETS.some((p) => p.w === w && p.h === h)) return "h";
    if (V_PRESETS.some((p) => p.w === w && p.h === h)) return "v";
    return "custom";
  }
  function presetsFor(o: Orient): ResPreset[] {
    return o === "v" ? V_PRESETS : H_PRESETS;
  }
  function nearestPreset(list: ResPreset[], w: number, h: number): ResPreset {
    const rotated = list.find((p) => p.w === h && p.h === w);
    if (rotated) return rotated;
    const area = w * h;
    return list.reduce(
      (best, p) => (Math.abs(p.w * p.h - area) < Math.abs(best.w * best.h - area) ? p : best),
      list[0],
    );
  }

  // svelte-ignore state_referenced_locally
  let orient = $state<Orient>(orientFor(form.width, form.height));

  const locked = $derived(isLive || form.useDefaultRes);

  function pickOrient(o: Orient): void {
    orient = o;
    if (o === "custom") return;
    const list = presetsFor(o);
    if (!list.some((p) => p.w === form.width && p.h === form.height)) {
      const p = nearestPreset(list, form.width, form.height);
      form.width = p.w;
      form.height = p.h;
    }
    commit();
  }
  function pickRes(w: number, h: number): void {
    form.width = w;
    form.height = h;
    commit();
  }
  function pickFpsPreset(p: number): void {
    form.fpsNum = p;
    form.fpsDen = 1;
    form.fpsCustom = false;
    commit();
  }
  // Custom FPS entered as a decimal (e.g. 59.94) -> num/den on commit.
  // svelte-ignore state_referenced_locally
  let customFps = $state(form.fpsDen > 1 ? +(form.fpsNum / form.fpsDen).toFixed(3) : form.fpsNum);
  function applyCustomFps(): void {
    const v = Number(customFps);
    if (!Number.isFinite(v) || v <= 0) return;
    if (Number.isInteger(v)) {
      form.fpsNum = v;
      form.fpsDen = 1;
    } else {
      // NTSC rates (59.94/29.97/23.976) reconstruct as .../1001; others use /1000.
      const den = Math.round(v * 1001) % 1000 === 0 ? 1001 : 1000;
      form.fpsNum = Math.round(v * den);
      form.fpsDen = den;
    }
    commit();
  }
</script>

<div class="body">
  {#if !isDefault}
    <UseDefaultStrip
      checked={form.useDefaultRes}
      label="Use Default resolution & frame rate"
      disabled={isLive}
      onchange={(v) => {
        form.useDefaultRes = v;
        commit();
      }}
    />
  {/if}

  <div class="field">
    <span class="flabel">Orientation</span>
    <div class="seg" class:dis={locked} role="tablist" aria-label="Orientation">
      {#each ORIENTS as o (o.value)}
        <button
          type="button"
          class="seg-btn"
          class:on={orient === o.value}
          role="tab"
          aria-selected={orient === o.value}
          disabled={locked}
          onclick={() => pickOrient(o.value)}>{o.label}</button
        >
      {/each}
    </div>
  </div>

  <div class="field">
    <span class="flabel">Resolution</span>
    {#if orient === "custom"}
      <div class="wh">
        <input
          type="number"
          min="1"
          max="16384"
          bind:value={form.width}
          disabled={locked}
          aria-label="Width"
          onchange={commit}
        />
        <span class="x">×</span>
        <input
          type="number"
          min="1"
          max="16384"
          bind:value={form.height}
          disabled={locked}
          aria-label="Height"
          onchange={commit}
        />
      </div>
    {:else}
      <div class="presets">
        {#each presetsFor(orient) as p (p.label)}
          <button
            type="button"
            class="chip"
            class:active={form.width === p.w && form.height === p.h}
            disabled={locked}
            onclick={() => pickRes(p.w, p.h)}>{p.label}</button
          >
        {/each}
      </div>
    {/if}
  </div>

  <div class="field">
    <span class="flabel">Frame Rate (FPS)</span>
    <div class="presets">
      {#each FPS_PRESETS as p (p)}
        <button
          type="button"
          class="chip"
          class:active={!form.fpsCustom && form.fpsNum === p && form.fpsDen === 1}
          disabled={locked}
          onclick={() => pickFpsPreset(p)}>{p}</button
        >
      {/each}
      <button
        type="button"
        class="chip"
        class:active={form.fpsCustom}
        disabled={locked}
        onclick={() => {
          form.fpsCustom = true;
        }}>Custom</button
      >
    </div>
    {#if form.fpsCustom}
      <div class="wh">
        <input
          type="number"
          min="1"
          max="1000"
          step="0.001"
          bind:value={customFps}
          disabled={locked}
          aria-label="Custom frame rate"
          onchange={applyCustomFps}
        />
        <span class="x">fps</span>
      </div>
    {/if}
  </div>

  <div class="field">
    <span class="flabel">Downscale Filter</span>
    <select bind:value={form.scaleType} disabled={locked} onchange={commit}>
      {#each scaleTypes as s (s.value)}
        <option value={s.value}>{s.label}</option>
      {/each}
    </select>
  </div>

  {#if isLive}
    <div class="lockrow">
      <Icon name="lock" size={13} /> Resolution &amp; frame rate are locked while this canvas is live.
    </div>
  {/if}
</div>

<style>
  .body {
    display: block;
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
  select {
    width: 100%;
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

  /* ---- orientation segmented control -------------------------------------- */
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

  .lockrow {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-top: 4px;
    padding: 9px 12px;
    border: var(--border-weight) solid var(--color-live);
    background: color-mix(in srgb, var(--color-live) 10%, transparent);
    color: var(--color-live);
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.04em;
  }
</style>
