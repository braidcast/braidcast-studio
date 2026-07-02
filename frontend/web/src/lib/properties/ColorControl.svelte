<script lang="ts">
  import type { ControlProps } from "./controls";
  let { prop, value, onChange }: ControlProps = $props();

  // OBS packs color as ABGR int: R | G<<8 | B<<16 | A<<24. Native <input
  // type=color> uses #rrggbb, so split RGB from the int and keep alpha separate
  // (only color_alpha exposes the alpha slider).
  const supportsAlpha = $derived(prop.type === "color_alpha");
  const packed = $derived(Number(value ?? 0) >>> 0);
  const r = $derived(packed & 0xff);
  const g = $derived((packed >> 8) & 0xff);
  const b = $derived((packed >> 16) & 0xff);
  const a = $derived((packed >> 24) & 0xff);

  const hex = $derived(
    "#" + [r, g, b].map((c) => c.toString(16).padStart(2, "0")).join(""),
  );

  function pack(rr: number, gg: number, bb: number, aa: number): number {
    return ((rr & 0xff) | ((gg & 0xff) << 8) | ((bb & 0xff) << 16) | ((aa & 0xff) << 24)) >>> 0;
  }

  function onHex(h: string) {
    const v = h.replace("#", "");
    const rr = parseInt(v.slice(0, 2), 16);
    const gg = parseInt(v.slice(2, 4), 16);
    const bb = parseInt(v.slice(4, 6), 16);
    onChange(prop.name, pack(rr, gg, bb, supportsAlpha ? a : 0xff));
  }

  function onAlpha(av: string) {
    const aa = Math.round((parseFloat(av) / 100) * 255);
    onChange(prop.name, pack(r, g, b, aa));
  }
</script>

<div class="color" title={prop.long_description ?? ""}>
  <input
    type="color"
    value={hex}
    disabled={!prop.enabled}
    oninput={(e) => onHex((e.currentTarget as HTMLInputElement).value)}
  />
  <span class="hex">{hex}</span>
  {#if supportsAlpha}
    <label class="alpha">
      α
      <input
        type="range"
        min="0"
        max="100"
        value={Math.round((a / 255) * 100)}
        disabled={!prop.enabled}
        oninput={(e) => onAlpha((e.currentTarget as HTMLInputElement).value)}
      />
    </label>
  {/if}
</div>

<style>
  .color {
    display: flex;
    align-items: center;
    gap: 10px;
  }
  input[type="color"] {
    width: 44px;
    height: 28px;
    padding: 0;
    border: 1px solid var(--color-border);
    background: none;
    cursor: pointer;
  }
  .hex {
    font-family: var(--mono, ui-monospace, monospace);
    font-size: 12px;
    color: var(--color-muted);
  }
  .alpha {
    display: flex;
    align-items: center;
    gap: 6px;
    flex: 1;
    color: var(--color-muted);
    font-size: 12px;
  }
  .alpha input {
    flex: 1;
  }
</style>
