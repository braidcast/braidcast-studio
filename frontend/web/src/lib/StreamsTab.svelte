<script lang="ts">
  import {
    obs,
    type StreamProfileInfo,
    type ServiceType,
    type StreamProfileCreateParams,
    type StreamProfileUpdateParams,
  } from "./bridge";
  import PropertyForm from "./properties/PropertyForm.svelte";

  let profiles = $state<StreamProfileInfo[]>([]);
  let serviceTypes = $state<ServiceType[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  // Inline add/edit form. `editingUuid` null => the add form; a uuid => editing.
  let formOpen = $state(false);
  let editingUuid = $state<string | null>(null);
  let fLabel = $state("");
  let fService = $state("");
  let saving = $state(false);
  let formError = $state<string | null>(null);

  async function loadProfiles() {
    try {
      profiles = await obs.call("streamProfile.list");
    } catch (e) {
      error = (e as Error).message;
    }
  }

  async function loadAll() {
    error = null;
    try {
      const [list, svc] = await Promise.all([obs.call("streamProfile.list"), obs.call("serviceTypes.list")]);
      profiles = list;
      serviceTypes = svc;
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void loadAll();
    // Live-refresh the list when any profile mutates (this tab or elsewhere).
    const off = obs.on("streamProfile.changed", () => void loadProfiles());
    return off;
  });

  function serviceName(id: string): string {
    return serviceTypes.find((s) => s.id === id)?.name ?? id ?? "—";
  }

  function openAdd() {
    editingUuid = null;
    fLabel = "";
    fService = serviceTypes[0]?.id ?? "rtmp_common";
    formError = null;
    formOpen = true;
  }

  function openEdit(p: StreamProfileInfo) {
    editingUuid = p.uuid;
    fLabel = p.label;
    fService = p.service || serviceTypes[0]?.id || "rtmp_common";
    formError = null;
    formOpen = true;
  }

  function closeForm() {
    formOpen = false;
  }

  const formValid = $derived(fLabel.trim().length > 0 && fService.length > 0);

  // The editing target's settings are edited live through PropertyForm
  // (kind:"service"), which persists each change immediately via properties.set.
  // Save here only commits the label/service header fields.
  async function save() {
    if (!formValid || saving) return;
    saving = true;
    formError = null;
    try {
      if (editingUuid) {
        const params: StreamProfileUpdateParams = {
          uuid: editingUuid,
          label: fLabel.trim(),
          service: fService,
        };
        const updated = await obs.call("streamProfile.update", params);
        // Keep the form open on the now-saved profile so its service settings
        // remain editable; refresh the list.
        editingUuid = updated.uuid;
      } else {
        const params: StreamProfileCreateParams = {
          label: fLabel.trim(),
          service: fService,
        };
        const created = await obs.call("streamProfile.create", params);
        // Switch the form into edit mode for the new profile so the user can fill
        // in its service settings (server/key) right away.
        editingUuid = created.uuid;
      }
      await loadProfiles();
    } catch (e) {
      formError = (e as Error).message;
    } finally {
      saving = false;
    }
  }

  async function remove(p: StreamProfileInfo) {
    try {
      await obs.call("streamProfile.remove", { uuid: p.uuid });
      if (editingUuid === p.uuid) formOpen = false;
      await loadProfiles();
    } catch (e) {
      error = (e as Error).message;
    }
  }

  async function setPrimary(p: StreamProfileInfo) {
    if (p.isPrimary) return;
    try {
      await obs.call("streamProfile.setPrimary", { uuid: p.uuid });
      await loadProfiles();
    } catch (e) {
      error = (e as Error).message;
    }
  }
</script>

<div class="streams">
  {#if error}<p class="error">{error}</p>{/if}

  {#if !loaded}
    <p class="dim">Loading stream profiles…</p>
  {:else}
    {#if profiles.length === 0}
      <p class="dim">No stream profiles yet. Add one to stream to a destination.</p>
    {:else}
      <ul class="list">
        {#each profiles as p (p.uuid)}
          <li class="row">
            <div class="info">
              <div class="line1">
                <span class="name">{p.label || p.platform}</span>
                {#if p.isPrimary}<span class="badge">Primary</span>{/if}
              </div>
              <div class="line2">
                {p.platform}
                <span class="sep">·</span>
                {serviceName(p.service)}
              </div>
            </div>
            <div class="rowactions">
              <button
                class="mini"
                title={p.isPrimary ? "Already primary" : "Set as primary"}
                disabled={p.isPrimary}
                onclick={() => void setPrimary(p)}>★</button
              >
              <button class="mini" title="Edit" onclick={() => openEdit(p)}>✎</button>
              <button class="mini danger" title="Remove" onclick={() => void remove(p)}>🗑</button>
            </div>
          </li>
        {/each}
      </ul>
    {/if}

    <div class="addbar">
      <button class="btn" onclick={openAdd}>+ Add Stream Profile</button>
    </div>
  {/if}

  {#if formOpen}
    <div class="form">
      <h4>{editingUuid ? "Edit Stream Profile" : "New Stream Profile"}</h4>

      <div class="field">
        <span class="flabel">Label</span>
        <!-- svelte-ignore a11y_autofocus -->
        <input type="text" bind:value={fLabel} autofocus placeholder="e.g. Main Channel" />
      </div>

      <div class="field">
        <span class="flabel">Service</span>
        <select bind:value={fService}>
          {#each serviceTypes as s (s.id)}
            <option value={s.id}>{s.name}</option>
          {/each}
        </select>
      </div>

      {#if formError}<p class="error">{formError}</p>{/if}

      <div class="actions">
        <button class="btn ghost" onclick={closeForm}>Close</button>
        <button class="btn primary" disabled={!formValid || saving} onclick={() => void save()}>
          {saving ? "Saving…" : editingUuid ? "Save" : "Create"}
        </button>
      </div>

      {#if editingUuid}
        <div class="settings">
          <div class="exp-head">Service settings</div>
          {#key editingUuid + ":" + fService}
            <PropertyForm kind="service" ref={editingUuid} />
          {/key}
        </div>
      {/if}
    </div>
  {/if}
</div>

<style>
  .streams {
    padding: 8px 0 4px;
  }
  .list {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    padding: 8px 10px;
    border: 1px solid var(--border);
    border-radius: 8px;
    background: var(--bg-sunken);
  }
  .info {
    min-width: 0;
  }
  .line1 {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .name {
    font-size: 13px;
    color: var(--text);
    font-weight: 500;
  }
  .badge {
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--color-accent-contrast);
    background: var(--accent);
    border-radius: 999px;
    padding: 1px 7px;
  }
  .line2 {
    font-size: 11px;
    color: var(--text-dim);
    margin-top: 2px;
  }
  .sep {
    margin: 0 4px;
  }
  .rowactions {
    display: flex;
    gap: 4px;
    flex-shrink: 0;
  }
  .mini {
    background: none;
    border: 1px solid var(--border);
    border-radius: 6px;
    color: var(--text-soft);
    cursor: pointer;
    font: inherit;
    font-size: 12px;
    padding: 4px 7px;
    line-height: 1;
  }
  .mini:hover:not(:disabled) {
    color: var(--text);
    background: var(--bg-raised);
  }
  .mini.danger:hover:not(:disabled) {
    color: var(--off, #d65a5a);
    border-color: var(--off, #d65a5a);
  }
  .mini:disabled {
    opacity: 0.35;
    cursor: default;
  }
  .addbar {
    margin-top: 12px;
  }
  .form {
    margin-top: 14px;
    padding: 14px;
    border: 1px solid var(--border);
    border-radius: 10px;
    background: var(--bg-raised);
  }
  .form h4 {
    margin: 0 0 12px;
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--text-dim);
  }
  .field {
    margin-bottom: 12px;
  }
  .flabel {
    display: block;
    font-size: 12px;
    color: var(--text-soft);
    margin-bottom: 6px;
  }
  input,
  select {
    background: var(--bg-sunken);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 7px 10px;
    color: var(--text);
    font: inherit;
    width: 100%;
  }
  input[type="text"],
  select {
    max-width: 340px;
  }
  input:focus,
  select:focus {
    outline: none;
    border-color: var(--accent);
  }
  .actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
    margin-top: 4px;
  }
  .settings {
    margin-top: 14px;
    padding-top: 12px;
    border-top: 1px solid var(--border);
  }
  .exp-head {
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--text-dim);
    margin-bottom: 10px;
  }
  .btn {
    border-radius: 6px;
    padding: 7px 14px;
    font: inherit;
    cursor: pointer;
    border: 1px solid var(--border);
    background: var(--bg-sunken);
    color: var(--text-soft);
  }
  .btn:hover:not(:disabled) {
    color: var(--text);
  }
  .btn.primary {
    background: var(--accent);
    border-color: var(--accent);
    color: var(--color-accent-contrast);
  }
  .btn.primary:disabled {
    opacity: 0.45;
    cursor: default;
  }
  .btn.ghost {
    background: none;
  }
  .dim {
    color: var(--text-dim);
    margin: 0;
  }
  .error {
    color: var(--off, #d65a5a);
    margin: 0 0 8px;
    font-size: 12px;
  }
</style>
