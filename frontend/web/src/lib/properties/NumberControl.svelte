<script lang="ts">
  import type { ControlProps } from "./controls";
  import type { IntProperty, FloatProperty } from "../bridge";
  let { prop, value, onChange }: ControlProps = $props();

  // Covers both int and float; the descriptor carries min/max/step + a
  // slider-vs-scroller hint and an optional unit suffix.
  const p = $derived(prop as IntProperty | FloatProperty);
  const num = $derived(Number(value ?? 0));
  const slider = $derived(
    (p.type === "int" ? (p as IntProperty).int_type : (p as FloatProperty).float_type) === "slider",
  );

  function emit(raw: string) {
    const v = prop.type === "int" ? parseInt(raw, 10) : parseFloat(raw);
    if (!Number.isNaN(v)) onChange(prop.name, v);
  }
</script>

<div class="num" title={prop.long_description ?? ""}>
  {#if slider}
    <input
      type="range"
      min={p.min}
      max={p.max}
      step={p.step || 1}
      value={num}
      disabled={!prop.enabled}
      oninput={(e) => emit((e.currentTarget as HTMLInputElement).value)}
    />
  {/if}
  <input
    class="spin"
    type="number"
    min={p.min}
    max={p.max}
    step={p.step || 1}
    value={num}
    disabled={!prop.enabled}
    oninput={(e) => emit((e.currentTarget as HTMLInputElement).value)}
  />
  {#if p.suffix}<span class="suffix">{p.suffix}</span>{/if}
</div>

<style>
  .num {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  input[type="range"] {
    flex: 1;
    min-width: 0;
  }
  .spin {
    width: 90px;
  }
  .suffix {
    color: var(--color-muted);
    font-size: 12px;
  }
</style>
