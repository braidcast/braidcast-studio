<script lang="ts">
  import type { ControlProps } from "$lib/properties/controls";
  import type { FontProperty, FontValue } from "$lib/api/bridge";
  let { prop, value, onChange }: ControlProps = $props();

  // flags bitmask: BOLD=1, ITALIC=2, UNDERLINE=4, STRIKEOUT=8.
  const BOLD = 1,
    ITALIC = 2,
    UNDERLINE = 4,
    STRIKEOUT = 8;

  const p = $derived(prop as FontProperty);
  const v = $derived((value ?? p.value) as FontValue | null);
  const face = $derived(v?.face ?? "");
  const size = $derived(v?.size ?? 0);
  const flags = $derived(v?.flags ?? 0);

  // Emit the FULL value object every change; `style` is preserved untouched
  // (libobs uses it to disambiguate weights the face name alone can't express).
  function emit(patch: Partial<FontValue>) {
    const next: FontValue = {
      face,
      size,
      flags,
      ...(v?.style != null ? { style: v.style } : {}),
      ...patch,
    };
    onChange(prop.name, next);
  }

  function toggleFlag(bit: number, on: boolean) {
    emit({ flags: on ? flags | bit : flags & ~bit });
  }
</script>

<div class="font" title={prop.long_description ?? ""}>
  <div class="row">
    <!-- Offline system-font enumeration isn't available in CEF, so face is a
         free-text field for the MVP rather than a picker of installed families. -->
    <input
      class="face"
      type="text"
      placeholder="font family"
      value={face}
      disabled={!prop.enabled}
      oninput={(e) => emit({ face: (e.currentTarget as HTMLInputElement).value })}
    />
    <input
      class="size"
      type="number"
      min="1"
      placeholder="size"
      value={size}
      disabled={!prop.enabled}
      oninput={(e) => {
        const n = parseInt((e.currentTarget as HTMLInputElement).value, 10);
        if (!Number.isNaN(n)) emit({ size: n });
      }}
    />
  </div>
  <div class="flags">
    <label class="flag">
      <input
        type="checkbox"
        checked={(flags & BOLD) !== 0}
        disabled={!prop.enabled}
        onchange={(e) => toggleFlag(BOLD, (e.currentTarget as HTMLInputElement).checked)}
      />
      <span>Bold</span>
    </label>
    <label class="flag">
      <input
        type="checkbox"
        checked={(flags & ITALIC) !== 0}
        disabled={!prop.enabled}
        onchange={(e) => toggleFlag(ITALIC, (e.currentTarget as HTMLInputElement).checked)}
      />
      <span>Italic</span>
    </label>
    <label class="flag">
      <input
        type="checkbox"
        checked={(flags & UNDERLINE) !== 0}
        disabled={!prop.enabled}
        onchange={(e) => toggleFlag(UNDERLINE, (e.currentTarget as HTMLInputElement).checked)}
      />
      <span>Underline</span>
    </label>
    <label class="flag">
      <input
        type="checkbox"
        checked={(flags & STRIKEOUT) !== 0}
        disabled={!prop.enabled}
        onchange={(e) => toggleFlag(STRIKEOUT, (e.currentTarget as HTMLInputElement).checked)}
      />
      <span>Strikeout</span>
    </label>
  </div>
</div>

<style>
  .font {
    display: flex;
    flex-direction: column;
    gap: 8px;
    min-width: 0;
  }
  .row {
    display: flex;
    gap: 6px;
  }
  .face {
    flex: 1;
    min-width: 0;
  }
  .size {
    width: 90px;
  }
  .flags {
    display: flex;
    flex-wrap: wrap;
    gap: 12px;
  }
  .flag {
    display: flex;
    align-items: center;
    gap: 6px;
    cursor: pointer;
    font-size: 12px;
    color: var(--color-muted);
  }
  input:disabled {
    opacity: 0.5;
  }
</style>
