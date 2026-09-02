<script lang="ts">
  import type { ControlProps } from "$lib/properties/controls";
  import type { IntProperty, FloatProperty } from "$lib/api/bridge";
  import Icon from "$lib/ui/Icon.svelte";
  let { prop, value, onChange }: ControlProps = $props();

  // Covers both int and float; the descriptor carries min/max/step + a
  // slider-vs-scroller hint and an optional unit suffix.
  const p = $derived(prop as IntProperty | FloatProperty);
  // Seed from the descriptor's default when the live value is absent so a real
  // default doesn't collapse to a misleading 0 (e.g. "0 s").
  const num = $derived(Number(value ?? p.value ?? 0));
  const step = $derived(p.step || 1);
  const slider = $derived((p.type === "int" ? p.int_type : p.float_type) === "slider");

  // A float property's value reaches us widened from the float32 the source keeps, so a
  // shader declaring 0.8 reads back as 0.800000011920928955 and the box renders all of it.
  // Show it at the precision the step grid can actually address instead. Display-only: the
  // stored value is left alone until the user edits, so nothing is silently rewritten by
  // merely opening a dialog.
  const decimals = $derived(prop.type === "int" ? 0 : Math.min(10, Math.max(0, Math.ceil(-Math.log10(step)))));
  const shown = $derived(Number(num.toFixed(decimals)));

  function clamp(v: number): number {
    if (typeof p.min === "number" && v < p.min) v = p.min;
    if (typeof p.max === "number" && v > p.max) v = p.max;
    return v;
  }

  function emit(raw: string | number) {
    const parsed = prop.type === "int" ? parseInt(String(raw), 10) : parseFloat(String(raw));
    if (!Number.isNaN(parsed)) onChange(prop.name, clamp(parsed));
  }

  // Steppers round to the step grid so a float doesn't accumulate drift -- stepping from
  // the displayed value rather than the raw one is what makes that true, otherwise every
  // nudge carries the widening error forward.
  function bump(dir: 1 | -1) {
    if (!prop.enabled) return;
    const next = clamp(shown + dir * step);
    onChange(prop.name, prop.type === "int" ? Math.round(next) : Number(next.toFixed(decimals)));
  }
</script>

<div class="numwrap" title={prop.long_description ?? ""}>
  {#if slider}
    <input
      class="cv-range"
      type="range"
      min={p.min}
      max={p.max}
      step={step}
      value={shown}
      disabled={!prop.enabled}
      oninput={(e) => emit((e.currentTarget as HTMLInputElement).value)}
    />
  {/if}
  <div class="cv-num cv-num--step" class:dis={!prop.enabled}>
    <input
      type="number"
      min={p.min}
      max={p.max}
      step={step}
      value={shown}
      disabled={!prop.enabled}
      oninput={(e) => emit((e.currentTarget as HTMLInputElement).value)}
    />
    {#if p.suffix}<span class="cv-num__u">{p.suffix}</span>{/if}
    <div class="cv-num__step">
      <button type="button" tabindex="-1" aria-label="Increment" disabled={!prop.enabled} onclick={() => bump(1)}>
        <Icon name="up" size={9} />
      </button>
      <button type="button" tabindex="-1" aria-label="Decrement" disabled={!prop.enabled} onclick={() => bump(-1)}>
        <Icon name="down" size={9} />
      </button>
    </div>
  </div>
</div>

<style>
  .numwrap {
    display: flex;
    align-items: center;
    gap: 12px;
  }
  .cv-range {
    flex: 1;
    min-width: 0;
  }
</style>
