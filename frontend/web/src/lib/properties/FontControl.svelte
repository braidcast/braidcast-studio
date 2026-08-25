<script lang="ts">
  import type { ControlProps } from "$lib/properties/controls";
  import type { FontProperty, FontValue } from "$lib/api/bridge";
  import FontDatalist from "$lib/ui/FontDatalist.svelte";
  let { prop, value, onChange }: ControlProps = $props();

  // Per mounted control: a properties view renders one of these per font property, and
  // two of them sharing an id would point both inputs at the first list in the document.
  //
  // Per control rather than hoisted to one list per form, which is what the overlay
  // fields panel does with its own: that panel holds its whole flat schema in one
  // component, so hoisting there costs nothing, while every control here is reached
  // through the single ControlProps contract (controls.ts) and through GroupControl's
  // recursion -- so a form-level list would have to thread an id down both, past eleven
  // controls with no use for it, to remove a duplicate libobs does not produce anyway: a
  // source declares one font property at most.
  const faceListId = $props.id();

  // PropertyRow renders the property's label as an unassociated <div>, so this input has
  // no accessible name of its own; `list` also promotes it to a combobox, which is worse
  // to land on unnamed than a textbox was. Qualified, because the row's label names the
  // whole font property while this input is only its family.
  const faceLabel = $derived(`${prop.label ?? prop.name} family`);

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
    <!-- Free text with the installed families as suggestions, not a picker: libobs takes
         whatever name is typed, and a font the host could not enumerate must stay
         reachable. -->
    <input
      class="face"
      type="text"
      placeholder="font family"
      aria-label={faceLabel}
      list={faceListId}
      value={face}
      disabled={!prop.enabled}
      oninput={(e) => emit({ face: (e.currentTarget as HTMLInputElement).value })}
    />
    <FontDatalist id={faceListId} />
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
