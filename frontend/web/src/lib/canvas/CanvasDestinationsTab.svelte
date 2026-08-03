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
  import Avatar from "$lib/ui/Avatar.svelte";
  import PlatformMark from "$lib/ui/PlatformMark.svelte";
  import ProfileSelect from "$lib/ui/ProfileSelect.svelte";
  import { STATE_COLOR_EXT } from "$lib/theme/stateColors";
  import {
    outputBindingStore,
    bindingDisplayName,
    isBindingDangling,
    isBindingUnset,
  } from "$lib/stores/outputBindingStore.svelte";
  import { bindingRowState, bindingRowDetail } from "$lib/stores/multistreamStatusStore.svelte";
  import { oauthStore } from "$lib/stores/oauthStore.svelte";
  import { profileName, platformLabel, profileAvatarUrl } from "$lib/utils/profileDisplay";
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

  // Profiles already bound to ANY canvas in this collection, omitted from the picker.
  //
  // Scoped to `bindings` and not to `rows`: reading only this canvas's rows let a
  // destination already bound elsewhere be offered here, and accepting the offer built
  // an edge that can never carry a stream. The engine permits exactly one ENABLED
  // binding per profile because one stream key is one live stream, so the second edge
  // is unreachable while the first is armed -- the picker was inviting the user to
  // create it anyway. Re-routing is outputBinding.update with a new canvasUuid, so
  // refusing the second edge costs nothing that the user could otherwise do.
  const boundProfileUuids = $derived(bindings.map((b) => b.profileUuid).filter(Boolean));

  // Per-canvas enabled = any binding enabled; the master toggle sets them all.
  const canvasEnabled = $derived(rows.some((b) => b.enabled));

  // A card names its destination the way the Streams list and the bind picker do --
  // avatar, profile name, platform -- because a binding row only carries the label
  // and "YouTube Main" alone does not say which channel it reaches. The profile is
  // the source of that detail, so it is looked up here rather than widened into
  // OutputBindingInfo, which would have to re-emit on every profile edit.
  $effect(() => oauthStore.subscribe());
  const profileByUuid = $derived(new Map(profiles.map((p) => [p.uuid, p])));
  function profileFor(b: OutputBindingInfo): StreamProfileInfo | undefined {
    return profileByUuid.get(b.profileUuid);
  }

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
      await outputBindingStore.setEnabled(rows.map((b) => b.uuid), target);
      onChanged();
    } catch (e) {
      error = (e as Error).message;
    }
  }
  async function toggleRow(b: OutputBindingInfo, enabled: boolean): Promise<void> {
    try {
      await outputBindingStore.setEnabled([b.uuid], enabled);
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
      Toggle-only. Each card pairs a global stream profile with this canvas. A canvas encodes only while
      <b>at least one</b> binding is enabled.
    </p>

    {#if rows.length === 0 && !adding}
      <p class="empty">No destinations bound to this canvas yet.</p>
    {/if}

    {#if adding}
      <div class="add-form">
        <span class="add-label">New destination</span>
        <ProfileSelect {profiles} hideUuids={boundProfileUuids} onSelect={(uuid) => void confirmAdd(uuid)} />
        <div class="add-actions">
          <button class="ghost" onclick={cancelAdd}>Cancel</button>
          <button class="ghost" onclick={() => void confirmAdd("")}>No destination</button>
        </div>
      </div>
    {/if}

    <div class="cards">
      {#each rows as b (b.uuid)}
        {@const s = bindingRowState(b, statusByBinding)}
        {@const p = profileFor(b)}
        {@const dangling = isBindingDangling(b.profileLabel)}
        {@const unset = isBindingUnset(b.profileLabel)}
        <div class="card" class:off={!b.enabled}>
          <div class="card-top">
            <span class="card-av">
              <Avatar
                url={p ? profileAvatarUrl(p) : ""}
                name={p ? profileName(p) : bindingDisplayName(b)}
                size={34}
              />
            </span>
            <div class="card-id">
              <span class="card-name" class:deleted={dangling} class:unset>
                {p ? profileName(p) : bindingDisplayName(b)}
              </span>
              <span class="card-plat">
                {#if p}
                  <span class="card-mark"><PlatformMark platform={p.platform} size={11} /></span>
                  <span class="card-plat-text">{platformLabel(p)}</span>
                {:else}
                  <span class="card-plat-text">{dangling ? "Profile deleted" : "Not bound to a destination"}</span>
                {/if}
              </span>
            </div>
            <span
              class="card-state"
              style:color={STATE_COLOR_EXT[s]}
              style:background={STATE_TAG_BG[s]}
              title={bindingRowDetail(b, statusByBinding) || undefined}
            >
              {titleState(s).toUpperCase()}
            </span>
          </div>
          <div class="card-foot">
            <label class="card-toggle">
              <ToggleSwitch size="sm" bind:checked={b.enabled} onchange={(v) => void toggleRow(b, v)} />
              <span>{b.enabled ? "Enabled" : "Disabled"}</span>
            </label>
            <button class="trash" title="Unbind destination" aria-label="Unbind destination" onclick={() => onRemove(b)}>
              <Icon name="trash" size={14} />
            </button>
          </div>
        </div>
      {/each}

      {#if !adding}
        <button class="add-card" onclick={startAdd}>
          <Icon name="plus" size={15} />
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
  /* A destination carries little enough text that a full-width row was almost all
     empty space. auto-fill rather than auto-fit so a single card keeps a card's
     width instead of stretching back across the pane. */
  .cards {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(248px, 1fr));
    gap: 10px;
  }
  .card {
    display: flex;
    flex-direction: column;
    gap: 12px;
    padding: 13px 14px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .card.off {
    background: var(--color-base);
  }
  .card-top {
    display: flex;
    align-items: flex-start;
    gap: 10px;
    min-width: 0;
  }
  .card-av {
    flex: 0 0 auto;
    display: inline-flex;
  }
  .card-id {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 4px;
    min-width: 0;
  }
  .card-name {
    font-size: 13.5px;
    font-weight: 600;
    letter-spacing: -0.01em;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .card-name.deleted {
    color: var(--color-live);
    font-style: italic;
  }
  .card-name.unset {
    color: var(--color-muted);
    font-style: italic;
    font-weight: 400;
  }
  .card-plat {
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
  }
  .card-mark {
    display: inline-flex;
    flex: 0 0 auto;
  }
  .card-plat-text {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .card-state {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.06em;
    padding: 2px 6px;
  }
  .card-foot {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    padding-top: 11px;
    border-top: var(--border-weight) solid var(--color-border-2);
  }
  .card-toggle {
    display: flex;
    align-items: center;
    gap: 8px;
    cursor: pointer;
    font-size: 11.5px;
    color: var(--color-dim);
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
  .add-card {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 8px;
    min-height: 108px;
    padding: 12px 16px;
    border: var(--border-weight) dashed var(--color-border);
    background: transparent;
    color: var(--color-dim);
    cursor: pointer;
    font-family: var(--font-ui);
    font-size: 12px;
  }
  .add-card:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  /* Full width above the grid: the picker's dropdown needs the room, and a
     combobox squeezed into one grid cell truncated its own option rows. */
  .add-form {
    display: flex;
    flex-direction: column;
    align-items: stretch;
    gap: 10px;
    margin-bottom: 12px;
    padding: 12px 14px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
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
