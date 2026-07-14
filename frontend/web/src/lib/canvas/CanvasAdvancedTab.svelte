<script lang="ts">
  import type { CanvasForm } from "$lib/canvas/canvasForm";
  import UseDefaultStrip from "$lib/canvas/UseDefaultStrip.svelte";

  interface Props {
    form: CanvasForm;
    isLive: boolean;
    isDefault: boolean;
    commit: () => void;
  }
  let { form, isLive, isDefault, commit }: Props = $props();

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

<div class="cv-body">
  {#if !isDefault}
    <UseDefaultStrip
      checked={form.colorUseDefault}
      label="Use Default color settings"
      disabled={isLive}
      onchange={(v) => {
        form.colorUseDefault = v;
        commit();
      }}
    />
  {/if}

  <div class="cv-field">
    <div class="cv-field__l">Color Format</div>
    <select class="cv-select" bind:value={form.colorFormat} disabled={dis || isLive} onchange={commit}>
      {#each colorFormats as f (f.value)}<option value={f.value}>{f.label}</option>{/each}
    </select>
    <div class="cv-field__h">8-bit NV12 for SDR; 10-bit P010 for HDR.</div>
  </div>
  <div class="cv-field">
    <div class="cv-field__l">Color Space</div>
    <select class="cv-select" bind:value={form.colorSpace} disabled={dis || isLive} onchange={commit}>
      {#each colorSpaces as cs (cs.value)}<option value={cs.value}>{cs.label}</option>{/each}
    </select>
  </div>
  <div class="cv-field">
    <div class="cv-field__l">Color Range</div>
    <div class="cv-seg" class:dis={dis || isLive} role="tablist" aria-label="Color range">
      {#each colorRanges as r (r.value)}
        <button
          type="button"
          class="cv-segbtn"
          class:on={form.colorRange === r.value}
          role="tab"
          aria-selected={form.colorRange === r.value}
          disabled={dis || isLive}
          onclick={() => {
            form.colorRange = r.value;
            commit();
          }}>{r.label}</button
        >
      {/each}
    </div>
    <div class="cv-field__h">Partial (limited) for streaming; Full for recording.</div>
  </div>
  <div class="cv-field">
    <div class="cv-field__l">SDR White Level</div>
    <div class="cv-num" class:dis={dis || isLive}>
      <input
        type="number"
        min="80"
        max="480"
        bind:value={form.sdrWhiteLevel}
        disabled={dis || isLive}
        aria-label="SDR white level"
        onchange={commit}
      />
      <span class="cv-num__u">nits</span>
    </div>
  </div>
  <div class="cv-field">
    <div class="cv-field__l">HDR Nominal Peak Level</div>
    <div class="cv-num" class:dis={dis || isLive}>
      <input
        type="number"
        min="400"
        max="10000"
        bind:value={form.hdrNominalPeakLevel}
        disabled={dis || isLive}
        aria-label="HDR nominal peak level"
        onchange={commit}
      />
      <span class="cv-num__u">nits</span>
    </div>
    <div class="cv-field__h">Only used when Color Space is a Rec. 2100 profile.</div>
  </div>
</div>
