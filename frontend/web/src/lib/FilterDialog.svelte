<script lang="ts">
  import { onMount } from "svelte";
  import Modal from "./Modal.svelte";
  import Icon from "./dock/Icon.svelte";
  import ToggleSwitch from "./ToggleSwitch.svelte";
  import { obs, type FilterInfo, type FilterType, type ReorderDirection } from "./bridge";
  import PropertyForm from "./properties/PropertyForm.svelte";
  import { filterDialogOpener, type FilterKind } from "./filterDialogOpener.svelte";

  interface Props {
    source: string;
    onClose: () => void;
  }
  let { source, onClose }: Props = $props();

  // The native preview overlay (a sibling HWND painted above CEF) is suspended by
  // the opener (openFilters) for this dialog's whole lifetime, so it never occludes
  // the modal. Suspending in the opener rather than here keeps the gate ref-count
  // from transiently hitting zero on a context-menu -> modal handoff.
  // Side effect: filter changes aren't visible live IN the dialog yet; that needs an
  // in-dialog preview surface (known follow-up). The effect IS visible in the main
  // preview once the dialog closes and the overlay re-asserts.

  // Which stream the picker offers types for. Audio sources ask for audio-only so
  // video filters never appear in their list.
  const kind = $derived<FilterKind>(filterDialogOpener.kind);

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
  // For an audio-only request, suppress the Video group entirely and list every audio
  // type (including dual-capability ones, which the video-wins rule would drop).
  const videoTypes = $derived(kind === "audio" ? [] : types.filter((t) => t.video));
  const audioTypes = $derived(
    kind === "audio" ? types.filter((t) => t.audio) : types.filter((t) => t.audio && !t.video),
  );

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
      types = await obs.call("filterTypes.list", { kind });
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
      // Swallow Escape so it cancels the rename without also reaching Modal's
      // window handler (which would close the whole dialog).
      e.stopPropagation();
      renamingUuid = null;
    }
  }

  // Modal owns Escape (always closes). While the add-filter picker is open, swallow
  // Escape so it cancels the picker's native dropdown without also closing the modal
  // — preserving the pre-Modal guard behavior.
  function onPickKey(e: KeyboardEvent) {
    if (e.key === "Escape") {
      e.stopPropagation();
    }
  }
</script>

<Modal title="Filters — {source}" {onClose} width={760}>
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
              <span class="enable-tog" title={f.enabled ? "Disable" : "Enable"}>
                <ToggleSwitch size="sm" checked={f.enabled} onchange={() => void setEnabled(f)} />
              </span>
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
                <button
                  class="dock-icon"
                  title="Move up"
                  aria-label="Move up"
                  disabled={idx === 0}
                  onclick={() => void reorder(f, "up")}
                >
                  <Icon name="up" size={12} />
                </button>
                <button
                  class="dock-icon"
                  title="Move down"
                  aria-label="Move down"
                  disabled={idx === filters.length - 1}
                  onclick={() => void reorder(f, "down")}
                >
                  <Icon name="down" size={12} />
                </button>
                <button class="dock-icon danger" title="Remove" aria-label="Remove" onclick={() => void remove(f)}>
                  <Icon name="trash" size={12} />
                </button>
              </span>
            </li>
          {/each}
        </ul>
      {/if}

      <div class="add">
        {#if picking}
          <select bind:value={pickType} onchange={addFilter} onkeydown={onPickKey} use:focusOnMount>
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
          <button class="add-btn" disabled={!typesLoaded} onclick={beginPick}>
            <Icon name="plus" size={12} /> Add Filter
          </button>
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

  {#snippet footer()}
    <button class="btn" onclick={onClose}>Close</button>
  {/snippet}
</Modal>

<style>
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
  .enable-tog {
    flex: 0 0 auto;
    display: inline-flex;
    align-items: center;
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

  .add-btn {
    width: 100%;
    height: auto;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    padding: 6px 12px;
    font-family: var(--font-ui);
    font-size: 11px;
    background: transparent;
    color: var(--color-text);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  .add-btn:hover:not(:disabled) {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .add-btn:disabled {
    opacity: 0.45;
    cursor: default;
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
