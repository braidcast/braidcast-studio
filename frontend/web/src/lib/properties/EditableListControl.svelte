<script lang="ts">
  import { obs } from "$lib/api/bridge";
  import type { ControlProps } from "$lib/properties/controls";
  import type { EditableListProperty, EditableListItem } from "$lib/api/bridge";
  import IconButton, { ICONBTN_FIELD } from "$lib/ui/IconButton.svelte";
  let { prop, value, onChange }: ControlProps = $props();

  const p = $derived(prop as EditableListProperty);
  const items = $derived(((value ?? p.value) as EditableListItem[]) ?? []);
  const allowsFiles = $derived(p.editable_list_type === "files" || p.editable_list_type === "files_and_urls");

  let draft = $state("");

  // Emit the full array on every mutation, preserving each existing item's
  // uuid/selected/hidden; new entries are created as { value } only.
  function commit(next: EditableListItem[]) {
    onChange(prop.name, next);
  }

  function setValue(idx: number, text: string) {
    commit(items.map((it, i) => (i === idx ? { ...it, value: text } : it)));
  }

  function remove(idx: number) {
    commit(items.filter((_, i) => i !== idx));
  }

  function move(idx: number, delta: number) {
    const target = idx + delta;
    if (target < 0 || target >= items.length) return;
    const next = items.slice();
    const [row] = next.splice(idx, 1);
    next.splice(target, 0, row);
    commit(next);
  }

  function addDraft() {
    const text = draft.trim();
    if (!text) return;
    commit([...items, { value: text }]);
    draft = "";
  }

  async function addFiles() {
    const { path } = await obs.call("dialog.openFile", {
      mode: "open",
      filter: p.filter ?? undefined,
      defaultPath: p.default_path ?? undefined,
    });
    if (path != null) commit([...items, { value: path }]);
  }
</script>

<div class="elist" title={prop.long_description ?? ""}>
  <div class="rows">
    {#each items as item, idx (item.uuid ?? idx)}
      <div class="row">
        <input
          type="text"
          value={item.value}
          disabled={!prop.enabled}
          oninput={(e) => setValue(idx, (e.currentTarget as HTMLInputElement).value)}
        />
        <IconButton
          icon="up"
          {...ICONBTN_FIELD}
          title="Move up"
          aria-label="Move up"
          disabled={!prop.enabled || idx === 0}
          onclick={() => move(idx, -1)}
        />
        <IconButton
          icon="down"
          {...ICONBTN_FIELD}
          title="Move down"
          aria-label="Move down"
          disabled={!prop.enabled || idx === items.length - 1}
          onclick={() => move(idx, 1)}
        />
        <IconButton
          icon="x"
          {...ICONBTN_FIELD}
          tone="live"
          danger
          title="Remove"
          aria-label="Remove"
          disabled={!prop.enabled}
          onclick={() => remove(idx)}
        />
      </div>
    {/each}
    {#if items.length === 0}
      <p class="empty">No entries</p>
    {/if}
  </div>

  <div class="add">
    <input
      type="text"
      placeholder="Add entry…"
      bind:value={draft}
      disabled={!prop.enabled}
      onkeydown={(e) => {
        if (e.key === "Enter") {
          e.preventDefault();
          addDraft();
        }
      }}
    />
    <button type="button" class="add-btn" disabled={!prop.enabled} onclick={addDraft}>Add</button>
    {#if allowsFiles}
      <button type="button" class="add-btn" disabled={!prop.enabled} onclick={() => void addFiles()}>
        Add Files…
      </button>
    {/if}
  </div>
</div>

<style>
  .elist {
    display: flex;
    flex-direction: column;
    gap: 8px;
    min-width: 0;
  }
  .rows {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .row {
    display: flex;
    gap: 4px;
    align-items: center;
  }
  .row input {
    flex: 1;
    min-width: 0;
  }
  .empty {
    margin: 0;
    color: var(--color-muted);
    font-size: 12px;
  }
  .add {
    display: flex;
    gap: 6px;
  }
  .add input {
    flex: 1;
    min-width: 0;
  }
  .add-btn {
    white-space: nowrap;
    flex: none;
  }
  button:disabled {
    opacity: 0.5;
  }
</style>
