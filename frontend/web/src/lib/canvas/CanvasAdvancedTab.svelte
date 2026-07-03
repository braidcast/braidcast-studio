<script lang="ts">
  import type { CanvasForm } from "./canvasForm";
  import UseDefaultStrip from "./UseDefaultStrip.svelte";

  interface Props {
    form: CanvasForm;
    isDefault: boolean;
    commit: () => void;
  }
  let { form, isDefault, commit }: Props = $props();

  const colorFormats = [
    { label: "NV12 (8-bit)", value: "NV12" },
    { label: "I420 (8-bit)", value: "I420" },
    { label: "I444 (8-bit)", value: "I444" },
    { label: "I010 (10-bit)", value: "I010" },
    { label: "P010 (10-bit)", value: "P010" },
    { label: "P216 (16-bit)", value: "P216" },
    { label: "P416 (16-bit)", value: "P416" },
    { label: "RGB (8-bit)", value: "RGB" },
  ];
  const colorSpaces = [
    { label: "601 (SDR)", value: "601" },
    { label: "709 (SDR)", value: "709" },
    { label: "2100PQ (HDR)", value: "2100PQ" },
    { label: "2100HLG (HDR)", value: "2100HLG" },
    { label: "sRGB", value: "sRGB" },
  ];
  const colorRanges = [
    { label: "Partial", value: "Partial" },
    { label: "Full", value: "Full" },
  ];

  const dis = $derived(form.colorUseDefault && !isDefault);
</script>

<div class="body">
  {#if !isDefault}
    <UseDefaultStrip
      checked={form.colorUseDefault}
      label="Use Default color settings"
      onchange={(v) => {
        form.colorUseDefault = v;
        commit();
      }}
    />
  {/if}

  <div class="field">
    <span class="flabel">Color Format</span>
    <select bind:value={form.colorFormat} disabled={dis} onchange={commit}>
      {#each colorFormats as f (f.value)}<option value={f.value}>{f.label}</option>{/each}
    </select>
  </div>
  <div class="field">
    <span class="flabel">Color Space</span>
    <select bind:value={form.colorSpace} disabled={dis} onchange={commit}>
      {#each colorSpaces as cs (cs.value)}<option value={cs.value}>{cs.label}</option>{/each}
    </select>
  </div>
  <div class="field">
    <span class="flabel">Color Range</span>
    <select bind:value={form.colorRange} disabled={dis} onchange={commit}>
      {#each colorRanges as r (r.value)}<option value={r.value}>{r.label}</option>{/each}
    </select>
  </div>
  <div class="field">
    <span class="flabel">SDR White Level</span>
    <div class="wh">
      <input
        type="number"
        min="80"
        max="480"
        bind:value={form.sdrWhiteLevel}
        disabled={dis}
        aria-label="SDR white level"
        onchange={commit}
      />
      <span class="x">nits</span>
    </div>
  </div>
  <div class="field">
    <span class="flabel">HDR Nominal Peak Level</span>
    <div class="wh">
      <input
        type="number"
        min="400"
        max="10000"
        bind:value={form.hdrNominalPeakLevel}
        disabled={dis}
        aria-label="HDR nominal peak level"
        onchange={commit}
      />
      <span class="x">nits</span>
    </div>
  </div>
</div>

<style>
  /* .field / .flabel / select / .wh / .x / input[type=number] ported verbatim from CanvasEditor.svelte. */
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
</style>
