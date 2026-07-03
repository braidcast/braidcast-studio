<script lang="ts">
  import type { ControlProps } from "./controls";
  import type { ListProperty } from "../bridge";
  let { prop, value, onChange }: ControlProps = $props();

  const p = $derived(prop as ListProperty);

  // A small choice set with short labels reads best as a segmented control (e.g.
  // rate control CBR/VBR/CQP); larger sets, or short sets whose labels are long
  // enough to overflow a button (Multipass, Tuning), stay a select. Both cutoffs
  // are one-line tunables.
  const SEGMENTED_MAX_OPTIONS = 4;
  const SEGMENTED_MAX_LABEL = 10;
  const segmented = $derived(
    (p.combo_type === "list" || p.combo_type === "radio") &&
      p.items.length > 0 &&
      p.items.length <= SEGMENTED_MAX_OPTIONS &&
      p.items.every((it) => (it.name ?? String(it.value)).length <= SEGMENTED_MAX_LABEL),
  );

  // <select> values are strings, but item values may be int/float/bool. Index by
  // position and report the item's real typed value back. Match the current
  // value to an item to seed the selection.
  const selectedIdx = $derived(p.items.findIndex((it) => it.value === value));

  function pickByIndex(idxStr: string) {
    const idx = parseInt(idxStr, 10);
    const item = p.items[idx];
    if (item) onChange(prop.name, item.value);
  }
</script>

{#if p.combo_type === "editable"}
  <!-- Editable combo: free text + datalist of suggestions (string format). -->
  <input
    type="text"
    class="cv-editable"
    list={`dl-${prop.name}`}
    value={value == null ? "" : String(value)}
    disabled={!prop.enabled}
    title={prop.long_description ?? ""}
    oninput={(e) => onChange(prop.name, (e.currentTarget as HTMLInputElement).value)}
  />
  <datalist id={`dl-${prop.name}`}>
    {#each p.items as item, idx (idx)}
      <option value={String(item.value)}>{item.name ?? ""}</option>
    {/each}
  </datalist>
{:else if segmented}
  <div class="cv-seg" class:dis={!prop.enabled} role="radiogroup" aria-label={prop.label ?? prop.name}>
    {#each p.items as item, idx (idx)}
      <button
        type="button"
        class="cv-segbtn"
        class:on={item.value === value}
        role="radio"
        aria-checked={item.value === value}
        disabled={!prop.enabled || item.disabled}
        onclick={() => onChange(prop.name, item.value)}
      >
        {item.name ?? String(item.value)}
      </button>
    {/each}
  </div>
{:else}
  <select
    class="cv-select"
    disabled={!prop.enabled}
    title={prop.long_description ?? ""}
    value={String(selectedIdx)}
    onchange={(e) => pickByIndex((e.currentTarget as HTMLSelectElement).value)}
  >
    {#each p.items as item, idx (idx)}
      <option value={String(idx)} disabled={item.disabled}>{item.name ?? String(item.value)}</option>
    {/each}
  </select>
{/if}

<style>
  .cv-editable {
    width: 100%;
    max-width: 460px;
  }
</style>
