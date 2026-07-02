<script lang="ts">
  import type { ControlProps } from "./controls";
  import type { FrameRateProperty, FrameRateValue } from "../bridge";
  let { prop, value, onChange }: ControlProps = $props();

  const p = $derived(prop as FrameRateProperty);
  const v = $derived((value ?? p.value) as FrameRateValue | null);
  const num = $derived(v?.numerator ?? 0);
  const den = $derived(v?.denominator ?? 1);

  // Common broadcast rates (label -> rational). The num/den pair is the source
  // of truth; selecting a preset just fills it in. "Custom…" leaves them as-is.
  const COMMON: { label: string; num: number; den: number }[] = [
    { label: "60", num: 60, den: 1 },
    { label: "59.94", num: 60000, den: 1001 },
    { label: "50", num: 50, den: 1 },
    { label: "30", num: 30, den: 1 },
    { label: "29.97", num: 30000, den: 1001 },
    { label: "25", num: 25, den: 1 },
    { label: "24", num: 24, den: 1 },
    { label: "23.976", num: 24000, den: 1001 },
  ];

  // Index of the matching preset, or -1 ("Custom…").
  const presetIdx = $derived(COMMON.findIndex((c) => c.num === num && c.den === den));

  function emit(n: number, d: number) {
    onChange(prop.name, { numerator: n, denominator: d } satisfies FrameRateValue);
  }

  function pickPreset(idxStr: string) {
    const idx = parseInt(idxStr, 10);
    const c = COMMON[idx];
    if (c) emit(c.num, c.den);
  }

  function setNum(raw: string) {
    const n = parseInt(raw, 10);
    if (!Number.isNaN(n)) emit(n, den);
  }

  function setDen(raw: string) {
    const d = parseInt(raw, 10);
    if (!Number.isNaN(d)) emit(num, d);
  }
</script>

<div class="frate" title={prop.long_description ?? ""}>
  <select
    disabled={!prop.enabled}
    value={String(presetIdx)}
    onchange={(e) => pickPreset((e.currentTarget as HTMLSelectElement).value)}
  >
    {#each COMMON as c, idx (idx)}
      <option value={String(idx)}>{c.label}</option>
    {/each}
    <option value="-1">Custom…</option>
  </select>

  <div class="rational">
    <input
      type="number"
      min="1"
      title="numerator"
      value={num}
      disabled={!prop.enabled}
      oninput={(e) => setNum((e.currentTarget as HTMLInputElement).value)}
    />
    <span class="slash">/</span>
    <input
      type="number"
      min="1"
      title="denominator"
      value={den}
      disabled={!prop.enabled}
      oninput={(e) => setDen((e.currentTarget as HTMLInputElement).value)}
    />
  </div>
</div>

<style>
  .frate {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 10px;
    min-width: 0;
  }
  .rational {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .rational input {
    width: 90px;
  }
  .slash {
    color: var(--color-muted);
  }
  input:disabled,
  select:disabled {
    opacity: 0.5;
  }
</style>
