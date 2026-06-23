<script lang="ts">
  import { onMount } from "svelte";
  import { obs, type FilterInfo, type FilterType, type ReorderDirection } from "./bridge";
  import PropertyForm from "./properties/PropertyForm.svelte";
  import { suspendPreview } from "./previewGate.svelte";

  interface Props {
    source: string;
    onClose: () => void;
  }
  let { source, onClose }: Props = $props();

  // This dialog overlaps the native preview overlay (a sibling HWND painted above
  // CEF), so suspend it while open -- otherwise the overlay would occlude the modal.
  // Side effect: filter changes aren't visible live IN the dialog yet; that needs an
  // in-dialog preview surface (known follow-up). The effect IS visible in the main
  // preview once the dialog closes and the overlay re-asserts.
  $effect(() => suspendPreview());

  let filters = $state<FilterInfo[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);
  let selectedUuid = $state<string | null>(null);

  // Add-filter picker state. Types are fetched once; the picker reveals on demand.
  let types = $state<FilterType[]>([]);
  let typesLoaded = $state(false);
  let picking = $state(false);
  let pickType = $state<string>("");

  let renamingUuid = $state<string | null>(null);
  let renameTo = $state("");

  function report(e: unknown) {
    error = (e as Error).message;
  }

  function focusOnMount(node: HTMLInputElement | HTMLSelectElement) {
    node.focus();
    if (node instanceof HTMLInputElement) {
      node.select();
    }
  }

  // Split into the two <optgroup>s the picker renders. A filter type can be both
  // video and audio; bucket it by its primary stream (video wins) so it appears once.
  const videoTypes = $derived(types.filter((t) => t.video));
  const audioTypes = $derived(types.filter((t) => t.audio && !t.video));

  async function loadFilters(keepSelection = true) {
    error = null;
    try {
      const list = await obs.call("filters.list", { source });
      filters = list;
      if (!keepSelection || selectedUuid === null || !list.some((f) => f.uuid === selectedUuid)) {
        selectedUuid = list.length > 0 ? list[0].uuid : null;
      }
    } catch (e) {
      report(e);
    } finally {
      loaded = true;
    }
  }

  async function loadTypes() {
    try {
      types = await obs.call("filterTypes.list", { kind: "all" });
    } catch (e) {
      report(e);
    } finally {
      typesLoaded = true;
    }
  }

  // Re-load filters whenever the target source changes. Depends ONLY on `source`;
  // the types fetch is intentionally kept out of this effect so its `typesLoaded`
  // write can't re-trigger a redundant filter load + spurious selection reset.
  $effect(() => {
    void source;
    loaded = false;
    void loadFilters(false);
  });

  // Filter types are dialog-lifetime constant; fetch once. The dialog is remounted
  // per open, so onMount runs exactly once per open.
  onMount(() => {
    void loadTypes();
  });

  function selectFilter(f: FilterInfo) {
    selectedUuid = f.uuid;
  }

  async function setEnabled(f: FilterInfo) {
    try {
      await obs.call("filters.setEnabled", { source, name: f.name, enabled: !f.enabled });
      await loadFilters();
    } catch (e) {
      report(e);
    }
  }

  async function reorder(f: FilterInfo, direction: ReorderDirection) {
    try {
      await obs.call("filters.reorder", { source, name: f.name, direction });
      await loadFilters();
    } catch (e) {
      report(e);
    }
  }

  async function remove(f: FilterInfo) {
    try {
      await obs.call("filters.remove", { source, name: f.name });
      const wasSelected = selectedUuid === f.uuid;
      // After a removal the selection may be gone; let loadFilters re-anchor it.
      await loadFilters(!wasSelected);
    } catch (e) {
      report(e);
    }
  }

  function beginPick() {
    picking = true;
    pickType = "";
  }

  async function addFilter() {
    const type = pickType;
    if (!type) {
      picking = false;
      return;
    }
    try {
      const created = await obs.call("filters.add", { source, type });
      // Only dismiss the picker once the add succeeds; on error keep it open so the
      // surfaced message is actionable and the user can retry.
      picking = false;
      await loadFilters();
      selectedUuid = created.uuid;
    } catch (e) {
      report(e);
    }
  }

  function beginRename(f: FilterInfo) {
    renamingUuid = f.uuid;
    renameTo = f.name;
  }

  async function commitRename() {
    const target = filters.find((f) => f.uuid === renamingUuid);
    const newName = renameTo.trim();
    renamingUuid = null;
    if (!target || !newName || newName === target.name) {
      return;
    }
    try {
      await obs.call("filters.rename", { source, name: target.name, newName });
      await loadFilters();
    } catch (e) {
      report(e);
    }
  }

  function onRenameKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      void commitRename();
    } else if (e.key === "Escape") {
      renamingUuid = null;
    }
  }

  function onKeydown(e: KeyboardEvent) {
    if (e.key === "Escape") {
      // Let an in-progress inline rename or the picker cancel first.
      if (renamingUuid !== null || picking) {
        return;
      }
      onClose();
    }
  }
</script>

<svelte:window onkeydown={onKeydown} />

<div
  class="modal-backdrop"
  role="presentation"
  onclick={(e) => {
    if (e.target === e.currentTarget) onClose();
  }}
>
  <div class="modal" role="dialog" aria-modal="true" aria-label="Filters">
    <header class="modal-head">
      <h3>Filters — {source}</h3>
      <button class="icon close" title="Close" onclick={onClose}>✕</button>
    </header>

    <div class="modal-body">
      {#if error}<p class="error">{error}</p>{/if}

      <div class="wrap">
        <!-- left: filter list + add control -->
        <div class="left">
          {#if !loaded}
            <p class="dim">Loading…</p>
          {:else if filters.length === 0}
            <p class="dim">No filters on this source. Add one to get started.</p>
          {:else}
            <ul class="dock-list">
              {#each filters as f, idx (f.uuid)}
                <li class="dock-row" class:sel={f.uuid === selectedUuid} class:dimmed={!f.enabled}>
                  <button
                    class="dock-icon"
                    title={f.enabled ? "Disable" : "Enable"}
                    aria-pressed={f.enabled}
                    onclick={() => void setEnabled(f)}
                  >
                    {f.enabled ? "☑" : "☐"}
                  </button>
                  {#if renamingUuid === f.uuid}
                    <input
                      class="inline"
                      bind:value={renameTo}
                      onkeydown={onRenameKey}
                      onblur={commitRename}
                      use:focusOnMount
                    />
                  {:else}
                    <button class="dock-label" onclick={() => selectFilter(f)} ondblclick={() => beginRename(f)}>
                      {f.name}
                    </button>
                  {/if}
                  <span class="dock-actions">
                    <button class="dock-icon" title="Move up" disabled={idx === 0} onclick={() => void reorder(f, "up")}
                      >▲</button
                    >
                    <button
                      class="dock-icon"
                      title="Move down"
                      disabled={idx === filters.length - 1}
                      onclick={() => void reorder(f, "down")}>▼</button
                    >
                    <button class="dock-icon danger" title="Remove" onclick={() => void remove(f)}>🗑</button>
                  </span>
                </li>
              {/each}
            </ul>
          {/if}

          <div class="add">
            {#if picking}
              <select bind:value={pickType} onchange={addFilter} use:focusOnMount>
                <option value="" disabled selected>Select a filter…</option>
                {#if videoTypes.length > 0}
                  <optgroup label="Video">
                    {#each videoTypes as t (t.id)}
                      <option value={t.id}>{t.name}</option>
                    {/each}
                  </optgroup>
                {/if}
                {#if audioTypes.length > 0}
                  <optgroup label="Audio">
                    {#each audioTypes as t (t.id)}
                      <option value={t.id}>{t.name}</option>
                    {/each}
                  </optgroup>
                {/if}
              </select>
            {:else}
              <button class="btn" disabled={!typesLoaded} onclick={beginPick}>＋ Add Filter</button>
            {/if}
          </div>
        </div>

        <!-- right: selected filter's obs_properties -->
        <div class="right">
          {#if selectedUuid}
            <PropertyForm kind="filter" ref={selectedUuid} />
          {:else}
            <p class="dim">No filter selected</p>
          {/if}
        </div>
      </div>
    </div>

    <footer class="modal-foot">
      <button class="btn ghost" onclick={onClose}>Close</button>
    </footer>
  </div>
</div>

<style>
  .modal-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.55);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 100;
    padding: 24px;
  }
  .modal {
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    width: min(760px, 100%);
    max-height: 86vh;
    display: flex;
    flex-direction: column;
    box-shadow: 0 18px 48px rgba(0, 0, 0, 0.5);
    font-family: var(--font-ui);
  }
  .modal-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 8px 11px;
    background: var(--color-surface);
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .modal-head h3 {
    margin: 0;
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-text);
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .modal-body {
    padding: 12px;
    overflow: auto;
  }
  .modal-foot {
    display: flex;
    justify-content: flex-end;
    padding: 8px 11px;
    border-top: var(--border-weight) solid var(--color-border);
  }

  .wrap {
    display: flex;
    gap: 12px;
    align-items: flex-start;
  }
  .left {
    flex: 0 0 240px;
    min-width: 0;
    display: flex;
    flex-direction: column;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
  }
  .right {
    flex: 1;
    min-width: 0;
  }

  /* The shared .dock-actions reveal on row hover; in this compact list show the
     reorder/remove controls on the selected row too, so the selection is editable
     without hovering. */
  .dock-row.sel .dock-actions {
    display: inline-flex;
  }

  .add {
    padding: 8px;
    border-top: var(--border-weight) solid var(--color-border);
  }

  .inline {
    flex: 1;
    min-width: 0;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-accent);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 3px 5px;
  }
  .inline:focus {
    outline: none;
  }
  .dock-icon.danger:hover:not(:disabled) {
    color: var(--color-live);
  }

  select {
    width: 100%;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 5px 8px;
  }
  select:focus {
    outline: none;
    border-color: var(--color-accent);
  }

  .btn {
    width: 100%;
    height: auto;
    padding: 6px 12px;
    font-family: var(--font-ui);
    font-size: 11px;
    border: var(--border-weight) solid var(--color-border);
    background: transparent;
    color: var(--color-text);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  .btn:hover:not(:disabled) {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .btn:disabled {
    opacity: 0.45;
    cursor: default;
  }
  .modal-foot .btn {
    width: auto;
  }
  .btn.ghost {
    background: none;
  }

  .icon {
    background: none;
    border: none;
    color: var(--color-muted);
    cursor: pointer;
    padding: 2px 4px;
    font-size: 13px;
    line-height: 1;
    height: auto;
  }
  .icon:hover {
    color: var(--color-text);
  }
  .dim {
    color: var(--color-muted);
    margin: 0;
    padding: 8px;
    font-size: 11px;
  }
  .error {
    color: var(--color-live);
    margin: 0 0 8px;
    font-size: 11px;
  }
</style>
