<script lang="ts">
  import {
    obs,
    type OutputBindingInfo,
    type StreamProfileInfo,
    type MultistreamStatus,
    type MultistreamState,
  } from "$lib/api/bridge";
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import ProfileSelect from "$lib/ui/ProfileSelect.svelte";
  import { STATE_COLOR_EXT } from "$lib/theme/stateColors";
  import { bindingDisplayName, isBindingDangling, isBindingUnset } from "$lib/stores/outputBindingStore.svelte";
  import { bindingRowState, bindingRowDetail } from "$lib/stores/multistreamStatusStore.svelte";
  import { titleState } from "$lib/utils/format";

  interface Props {
    canvasUuid: string;
    bindings: OutputBindingInfo[];
    profiles: StreamProfileInfo[];
    statusByBinding: Map<string, MultistreamStatus>;
    onChanged: () => void;
    onRemove: (b: OutputBindingInfo) => void;
  }
  let { canvasUuid, bindings, profiles, statusByBinding, onChanged, onRemove }: Props = $props();

  let adding = $state(false);
  let error = $state<string | null>(null);

  const rows = $derived(bindings.filter((b) => b.canvasUuid === canvasUuid));

  // Profiles already bound to this canvas — omitted from the picker so a
  // destination can't be double-bound.
  const boundProfileUuids = $derived(rows.map((b) => b.profileUuid).filter(Boolean));

  // Per-canvas enabled = any binding enabled; the master toggle sets them all.
  const canvasEnabled = $derived(rows.some((b) => b.enabled));

  const STATE_TAG_BG: Record<MultistreamState | "disabled", string> = {
    disabled: "color-mix(in srgb, var(--color-muted) 10%, transparent)",
    idle: "color-mix(in srgb, var(--color-muted) 12%, transparent)",
    connecting: "color-mix(in srgb, var(--meter-yellow) 14%, transparent)",
    reconnecting: `color-mix(in srgb, ${STATE_COLOR_EXT.reconnecting} 14%, transparent)`,
    live: "color-mix(in srgb, var(--meter-green) 14%, transparent)",
    error: "color-mix(in srgb, var(--color-live) 14%, transparent)",
  };

  async function toggleCanvas(): Promise<void> {
    if (rows.length === 0) return;
    const target = !canvasEnabled;
    try {
      await Promise.all(rows.map((b) => obs.call("outputBinding.setEnabled", { uuid: b.uuid, enabled: target })));
      onChanged();
    } catch (e) {
      error = (e as Error).message;
    }
  }
  async function toggleRow(b: OutputBindingInfo, enabled: boolean): Promise<void> {
    try {
      await obs.call("outputBinding.setEnabled", { uuid: b.uuid, enabled });
      onChanged();
    } catch (e) {
      error = (e as Error).message;
      // Revert the optimistic toggle (checked is two-way bound to b.enabled).
      b.enabled = !enabled;
    }
  }

  function startAdd(): void {
    adding = true;
  }
  function cancelAdd(): void {
    adding = false;
  }
  async function confirmAdd(profileUuid: string): Promise<void> {
    try {
      await obs.call("outputBinding.create", {
        canvasUuid,
        ...(profileUuid ? { profileUuid } : {}),
      });
      adding = false;
      onChanged();
    } catch (e) {
      error = (e as Error).message;
    }
  }
</script>

<div class="cv-body">
  {#if error}<p class="err">{error}</p>{/if}

  <section class="section">
    <div class="sec-bar">
      {#if rows.length > 0}
        <span class="toggle-wrap" title={canvasEnabled ? "Disable all" : "Enable all"}>
          <ToggleSwitch size="sm" checked={canvasEnabled} onchange={() => void toggleCanvas()} />
        </span>
      {/if}
      <h3 class="sec-head">Output Bindings for this Canvas</h3>
      <span class="sec-count">{rows.filter((b) => b.enabled).length}/{rows.length} enabled</span>
    </div>
    <p class="sec-hint">
      Toggle-only. Each row pairs a global stream profile with this canvas. A canvas encodes only while
      <b>at least one</b> binding is enabled.
    </p>

    {#if rows.length === 0 && !adding}
      <p class="empty">No destinations bound to this canvas yet.</p>
    {/if}

    <div class="rows">
      {#each rows as b (b.uuid)}
        {@const s = bindingRowState(b, statusByBinding)}
        <div class="row" class:off={!b.enabled}>
          <span class="toggle-wrap" title={b.enabled ? "Disable" : "Enable"}>
            <ToggleSwitch size="sm" bind:checked={b.enabled} onchange={(v) => void toggleRow(b, v)} />
          </span>
          <div class="row-col">
            <div class="row-line1">
              <span class="row-name" class:deleted={isBindingDangling(b.profileLabel)} class:unset={isBindingUnset(b.profileLabel)}>
                {bindingDisplayName(b)}
              </span>
              <span
                class="row-state"
                style:color={STATE_COLOR_EXT[s]}
                style:background={STATE_TAG_BG[s]}
                title={bindingRowDetail(b, statusByBinding) || undefined}
              >
                {titleState(s).toUpperCase()}
              </span>
            </div>
          </div>
          <button
            class="trash"
            title="Remove destination"
            aria-label="Remove destination"
            onclick={() => onRemove(b)}
          >
            <Icon name="trash" size={14} />
          </button>
        </div>
      {/each}

      {#if adding}
        <div class="row add-form">
          <span class="add-label">New destination</span>
          <ProfileSelect
            {profiles}
            hideUuids={boundProfileUuids}
            onSelect={(uuid) => void confirmAdd(uuid)}
          />
          <div class="add-actions">
            <button class="ghost" onclick={cancelAdd}>Cancel</button>
            <button class="ghost" onclick={() => void confirmAdd("")}>No destination</button>
          </div>
        </div>
      {:else}
        <button class="add-tile-row" onclick={startAdd}>
          <Icon name="plus" size={13} />
          <span>Bind destination</span>
        </button>
      {/if}
    </div>
  </section>
</div>

<style>
  .err {
    margin: 0 0 12px;
    color: var(--color-live);
    font-size: 12px;
  }
  .section {
    margin-bottom: 0;
  }
  .sec-head {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.09em;
    color: var(--color-dim);
  }
  .sec-hint {
    margin: 0 0 14px;
    font-size: 10.5px;
    color: var(--color-muted);
    line-height: 1.4;
    max-width: 60ch;
  }
  .sec-hint b {
    color: var(--color-dim);
    font-weight: 500;
  }
  .sec-count {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    color: var(--color-muted);
  }
  .sec-bar {
    display: flex;
    align-items: center;
    gap: 10px;
    padding-bottom: 10px;
    border-bottom: var(--border-weight) solid var(--color-border-2);
    margin-bottom: 12px;
  }
  .empty {
    margin: 4px 0 12px;
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
  }
  .rows {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 12px 14px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .row.off {
    background: var(--color-base);
  }
  .row-col {
    flex: 1;
    min-width: 0;
  }
  .row-line1 {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .row-name {
    font-size: 14px;
    font-weight: 500;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .row-name.deleted {
    color: var(--color-live);
    font-style: italic;
  }
  .row-name.unset {
    color: var(--color-muted);
    font-style: italic;
    font-weight: 400;
  }
  .row-state {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.06em;
    padding: 2px 6px;
  }
  .trash {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    justify-content: center;
    width: 28px;
    height: 26px;
    padding: 0;
    background: none;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-muted);
    cursor: pointer;
  }
  .trash:hover {
    color: var(--color-live);
    border-color: var(--color-live);
  }
  .toggle-wrap {
    flex: 0 0 auto;
    display: inline-flex;
    align-items: center;
  }
  .add-tile-row {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    padding: 12px 16px;
    border: var(--border-weight) dashed var(--color-border);
    background: transparent;
    color: var(--color-dim);
    cursor: pointer;
    font-family: var(--font-ui);
    font-size: 12px;
  }
  .add-tile-row:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .add-form {
    flex-direction: column;
    align-items: stretch;
    gap: 10px;
  }
  .add-label {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .add-actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
  }
  .add-actions .ghost {
    padding: 7px 14px;
    background: none;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    cursor: pointer;
    font: inherit;
    font-size: 12px;
  }
  .add-actions .ghost:hover {
    color: var(--color-text);
  }
</style>
