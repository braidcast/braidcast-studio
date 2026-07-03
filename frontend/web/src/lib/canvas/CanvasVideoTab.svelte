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
    sub: string;
    w: number;
    h: number;
  }

  const H_PRESETS: ResPreset[] = [
    { label: "3840×2160", sub: "4K UHD", w: 3840, h: 2160 },
    { label: "2560×1440", sub: "QHD", w: 2560, h: 1440 },
    { label: "1920×1080", sub: "1080p FHD", w: 1920, h: 1080 },
    { label: "1600×900", sub: "900p HD", w: 1600, h: 900 },
    { label: "1280×720", sub: "720p HD", w: 1280, h: 720 },
  ];
  const V_PRESETS: ResPreset[] = [
    { label: "2160×3840", sub: "4K UHD", w: 2160, h: 3840 },
    { label: "1440×2560", sub: "QHD", w: 1440, h: 2560 },
    { label: "1080×1920", sub: "1080p FHD", w: 1080, h: 1920 },
    { label: "900×1600", sub: "900p HD", w: 900, h: 1600 },
    { label: "720×1280", sub: "720p HD", w: 720, h: 1280 },
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

<div class="cv-body">
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

  <div class="cv-field">
    <div class="cv-field__l">Orientation</div>
    <div class="cv-seg cv-seg--orient" class:dis={locked} role="tablist" aria-label="Orientation">
      {#each ORIENTS as o (o.value)}
        <button
          type="button"
          class="cv-segbtn"
          class:on={orient === o.value}
          role="tab"
          aria-selected={orient === o.value}
          disabled={locked}
          onclick={() => pickOrient(o.value)}
        >
          <svg class="cv-orient-ph" viewBox="0 0 28 26" fill="none" stroke="currentColor" stroke-width="1.6">
            {#if o.value === "h"}
              <rect x="3.5" y="7" width="21" height="12" />
            {:else if o.value === "v"}
              <rect x="9" y="3.5" width="10" height="19" />
            {:else}
              <rect x="4.5" y="6" width="19" height="14" stroke-dasharray="3 2.5" />
            {/if}
          </svg>
          <span>{o.label}</span>
        </button>
      {/each}
    </div>
    <div class="cv-field__h">Presets below switch between landscape and portrait ladders.</div>
  </div>

  <div class="cv-field">
    <div class="cv-field__l">Resolution</div>
    {#if orient === "custom"}
      <div class="cv-numrow">
        <div class="cv-num" class:dis={locked}>
          <input
            type="number"
            min="1"
            max="16384"
            bind:value={form.width}
            disabled={locked}
            aria-label="Width"
            onchange={commit}
          />
          <span class="cv-num__u">W</span>
        </div>
        <span class="slash">×</span>
        <div class="cv-num" class:dis={locked}>
          <input
            type="number"
            min="1"
            max="16384"
            bind:value={form.height}
            disabled={locked}
            aria-label="Height"
            onchange={commit}
          />
          <span class="cv-num__u">H</span>
        </div>
      </div>
    {:else}
      <div class="cv-pgrid" class:dis={locked}>
        {#each presetsFor(orient) as p (p.label)}
          <button
            type="button"
            class="cv-preset"
            class:on={form.width === p.w && form.height === p.h}
            disabled={locked}
            onclick={() => pickRes(p.w, p.h)}
          >
            <span class="cv-preset__r">{p.label}</span>
            <span class="cv-preset__t">{p.sub}</span>
          </button>
        {/each}
      </div>
    {/if}
    <div class="cv-field__h">
      The canvas is composited and encoded at this size. <b>Independent per canvas.</b>
    </div>
  </div>

  <div class="cv-field">
    <div class="cv-field__l">Frame Rate</div>
    <div class="cv-seg" class:dis={locked} role="tablist" aria-label="Frame rate">
      {#each FPS_PRESETS as p (p)}
        <button
          type="button"
          class="cv-segbtn"
          class:on={!form.fpsCustom && form.fpsNum === p && form.fpsDen === 1}
          disabled={locked}
          onclick={() => pickFpsPreset(p)}>{p}</button
        >
      {/each}
      <button
        type="button"
        class="cv-segbtn"
        class:on={form.fpsCustom}
        disabled={locked}
        onclick={() => {
          form.fpsCustom = true;
        }}>Custom</button
      >
    </div>
    {#if form.fpsCustom}
      <div class="customnum cv-num" class:dis={locked}>
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
        <span class="cv-num__u">fps</span>
      </div>
    {/if}
    <div class="cv-field__h">Pick a preset, or Custom to enter any value.</div>
  </div>

  <div class="cv-field">
    <div class="cv-field__l">Downscale Filter</div>
    <select class="cv-select" bind:value={form.scaleType} disabled={locked} onchange={commit}>
      {#each scaleTypes as s (s.value)}
        <option value={s.value}>{s.label}</option>
      {/each}
    </select>
  </div>

  {#if isLive}
    <div class="cv-lockrow">
      <Icon name="lock" size={13} /> Resolution &amp; frame rate are locked while this canvas is live.
    </div>
  {/if}
</div>

<style>
  .customnum {
    margin-top: 9px;
  }
</style>
