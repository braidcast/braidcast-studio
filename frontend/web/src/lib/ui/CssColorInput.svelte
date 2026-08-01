<script lang="ts">
  // Editor for a CSS colour string that carries an alpha channel. <input type="color">
  // is #rrggbb-only, so opacity rides a separate range and the two recombine on the way
  // out. Fully controlled and silent until the user moves something: a translucent value
  // that is merely displayed is written back byte-identical, and editing only the swatch
  // re-emits the parsed alpha verbatim rather than the slider's quantised position. A
  // string this control cannot represent (a keyword, a gradient, a var()) degrades to a
  // raw text box, because flattening it to opaque black is how the alpha was lost before.
  import { clamp } from "$lib/utils/clamp";

  let {
    value,
    onChange,
    ariaLabel,
  }: {
    value: string;
    onChange: (v: string) => void;
    ariaLabel?: string;
  } = $props();

  interface Rgba {
    r: number;
    g: number;
    b: number;
    a: number;
  }

  const HEX_RE = /^#([0-9a-f]+)$/i;
  const RGB_RE = /^rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(?:,\s*([0-9.]+)\s*)?\)$/i;

  function channel(v: number): number {
    return Math.round(clamp(v, 0, 255));
  }

  function parse(raw: string): Rgba | null {
    const s = raw.trim();
    const hex = HEX_RE.exec(s);
    if (hex) {
      const h = hex[1];
      const wide = h.length === 6 || h.length === 8;
      if (!wide && h.length !== 3 && h.length !== 4) {
        return null;
      }
      const step = wide ? 2 : 1;
      const at = (i: number): number => {
        const part = h.slice(i * step, i * step + step);
        return parseInt(wide ? part : part + part, 16);
      };
      const withAlpha = h.length === 4 || h.length === 8;
      return { r: at(0), g: at(1), b: at(2), a: withAlpha ? at(3) / 255 : 1 };
    }
    const rgb = RGB_RE.exec(s);
    if (!rgb) {
      return null;
    }
    const parts = [Number(rgb[1]), Number(rgb[2]), Number(rgb[3])];
    const a = rgb[4] === undefined ? 1 : Number(rgb[4]);
    if (parts.some((n) => !Number.isFinite(n)) || !Number.isFinite(a)) {
      return null;
    }
    return { r: channel(parts[0]), g: channel(parts[1]), b: channel(parts[2]), a: clamp(a, 0, 1) };
  }

  function toHex(c: Rgba): string {
    return "#" + [c.r, c.g, c.b].map((n) => n.toString(16).padStart(2, "0")).join("");
  }

  // Opaque values keep the #rrggbb form the shipped widget defaults use; anything
  // translucent widens to rgba(), written spaceless to match that same corpus.
  function serialize(c: Rgba): string {
    return c.a >= 1 ? toHex(c) : `rgba(${c.r},${c.g},${c.b},${c.a})`;
  }

  const parsed = $derived(parse(value));
  const alphaPct = $derived(parsed ? Math.round(parsed.a * 100) : 100);

  function setHex(h: string): void {
    const next = parse(h);
    if (next && parsed) {
      onChange(serialize({ ...next, a: parsed.a }));
    }
  }

  function setAlpha(pct: string): void {
    if (parsed) {
      onChange(serialize({ ...parsed, a: clamp(Number(pct), 0, 100) / 100 }));
    }
  }
</script>

{#if parsed}
  <div class="cc">
    <label class="swatch" style:--fill={serialize(parsed)}>
      <input type="color" value={toHex(parsed)} aria-label={ariaLabel} oninput={(e) => setHex(e.currentTarget.value)} />
    </label>
    <input
      class="alpha"
      type="range"
      min="0"
      max="100"
      value={alphaPct}
      aria-label={ariaLabel ? `${ariaLabel} opacity` : "Opacity"}
      oninput={(e) => setAlpha(e.currentTarget.value)}
    />
    <span class="pct">{alphaPct}%</span>
  </div>
{:else}
  <input
    class="raw"
    type="text"
    {value}
    aria-label={ariaLabel}
    oninput={(e) => onChange(e.currentTarget.value)}
  />
{/if}

<style>
  .cc {
    display: flex;
    align-items: center;
    gap: 10px;
    width: 100%;
  }
  /* The visible chip is the label, not the native picker: <input type="color"> renders
     its own opaque swatch, which is what made a transparent value look like black. The
     real colour sits over a checker so 0% alpha reads as transparent, and the picker
     lies invisible on top to keep the click target. */
  .swatch {
    position: relative;
    flex: 0 0 auto;
    width: 46px;
    height: 30px;
    border: var(--border-weight) solid var(--color-border);
    cursor: pointer;
    background-image: linear-gradient(var(--fill), var(--fill)),
      repeating-conic-gradient(var(--color-muted) 0% 25%, var(--color-base) 0% 50%);
    background-size:
      100% 100%,
      10px 10px;
  }
  .swatch input {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    padding: 0;
    border: 0;
    opacity: 0;
    cursor: pointer;
  }
  .alpha {
    flex: 1;
    min-width: 60px;
  }
  .pct {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .raw {
    width: 100%;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-mono);
    font-size: 12px;
    padding: 6px 8px;
  }
  .raw:focus {
    outline: none;
    border-color: var(--color-accent);
  }
</style>
