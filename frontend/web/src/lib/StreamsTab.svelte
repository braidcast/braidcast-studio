<script lang="ts">
  import {
    obs,
    type StreamProfileInfo,
    type ServiceType,
    type StreamProfileCreateParams,
    type StreamProfileUpdateParams,
    type OAuthProvider,
    type OAuthStatus,
  } from "./bridge";
  import PropertyForm from "./properties/PropertyForm.svelte";
  import { openOAuthConnect } from "./oauthConnectOpener.svelte";

  let profiles = $state<StreamProfileInfo[]>([]);
  let serviceTypes = $state<ServiceType[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  // Platform OAuth: the providers that support account connection (empty in a build
  // without a client id) + the per-profile link status. Drives the dual-path
  // Connection section and the tile "linked" chips.
  let providers = $state<OAuthProvider[]>([]);
  let statuses = $state<OAuthStatus[]>([]);

  // Inline add/edit form. `editingUuid` null => the add form; a uuid => editing.
  let formOpen = $state(false);
  let editingUuid = $state<string | null>(null);
  let editingProfile = $state<StreamProfileInfo | null>(null);
  let fLabel = $state("");
  let fService = $state("");
  let saving = $state(false);
  let formError = $state<string | null>(null);
  // Connection sub-path: "connect" (OAuth account) vs "key" (manual stream key).
  let connMode = $state<"connect" | "key">("key");

  async function loadProfiles() {
    try {
      profiles = await obs.call("streamProfile.list");
      // Keep the open form's profile snapshot fresh so its derived provider/platform
      // re-resolves after a save or an external change.
      if (editingUuid) {
        editingProfile = profiles.find((p) => p.uuid === editingUuid) ?? editingProfile;
      }
    } catch (e) {
      error = (e as Error).message;
    }
  }

  async function loadOAuth() {
    try {
      const [provs, stats] = await Promise.all([obs.call("oauth.providers"), obs.call("oauth.status")]);
      providers = provs;
      statuses = stats;
    } catch {
      // Non-fatal: a build without OAuth support leaves both empty (key-only).
    }
  }

  async function refreshStatus() {
    try {
      statuses = await obs.call("oauth.status");
    } catch {
      // Ignore; the next mount/load reconciles.
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

  $effect(() => {
    void loadOAuth();
    // Re-fetch link status whenever a profile connects/disconnects (any window).
    const off = obs.on("oauth.status", () => void refreshStatus());
    return off;
  });

  function serviceName(id: string): string {
    return serviceTypes.find((s) => s.id === id)?.name ?? id ?? "—";
  }

  // Which OAuth provider (if any) applies to a profile. Data lookup, not branches:
  // match the profile's display platform against a provider's id or displayName so
  // Kick/YouTube slot in by registering a provider, no code change here.
  function providerForProfile(p: StreamProfileInfo): OAuthProvider | null {
    const plat = (p.platform || "").trim().toLowerCase();
    if (!plat) {
      return null;
    }
    return providers.find((pv) => pv.id.toLowerCase() === plat || pv.displayName.toLowerCase() === plat) ?? null;
  }

  function connectedStatusFor(uuid: string): OAuthStatus | null {
    return statuses.find((s) => s.profileUuid === uuid && s.connected) ?? null;
  }

  // The provider + connection state for the profile in the open edit form.
  const editingProvider = $derived(editingProfile ? providerForProfile(editingProfile) : null);
  const connectedStatus = $derived(editingUuid ? connectedStatusFor(editingUuid) : null);

  function openAdd() {
    editingUuid = null;
    editingProfile = null;
    fLabel = "";
    fService = serviceTypes[0]?.id ?? "rtmp_common";
    connMode = "key";
    formError = null;
    formOpen = true;
  }

  function openEdit(p: StreamProfileInfo) {
    editingUuid = p.uuid;
    editingProfile = p;
    fLabel = p.label;
    fService = p.service || serviceTypes[0]?.id || "rtmp_common";
    // Default to Connect only when an account is already linked; otherwise (incl. no
    // provider) start on the stream-key path.
    connMode = providerForProfile(p) && connectedStatusFor(p.uuid) ? "connect" : "key";
    formError = null;
    formOpen = true;
  }

  function connect() {
    if (!editingUuid || !editingProvider) {
      return;
    }
    openOAuthConnect({
      profileUuid: editingUuid,
      providerId: editingProvider.id,
      platformName: editingProvider.displayName,
    });
  }

  async function disconnect() {
    if (!editingUuid) {
      return;
    }
    try {
      await obs.call("oauth.disconnect", { profileUuid: editingUuid });
      await refreshStatus();
    } catch (e) {
      formError = (e as Error).message;
    }
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
      <ul class="grid">
        {#each profiles as p (p.uuid)}
          <li class="tile">
            <button class="tile-body" onclick={() => openEdit(p)}>
              <div class="info">
                <div class="line1">
                  <span class="name">{p.label || p.platform}</span>
                  {#if p.isPrimary}<span class="badge">Primary</span>{/if}
                  {#if providerForProfile(p)}
                    {#if connectedStatusFor(p.uuid)}
                      <span class="chip ok">✓ linked</span>
                    {:else}
                      <span class="chip key">key only</span>
                    {/if}
                  {/if}
                </div>
                <div class="line2">
                  {p.platform}
                  <span class="sep">·</span>
                  {serviceName(p.service)}
                </div>
              </div>
            </button>
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

      {#if editingUuid}
        <div class="sect">
          <div class="exp-head">Connection</div>

          {#if editingProvider}
            <div class="seg">
              <button class="cell" class:on={connMode === "connect"} onclick={() => (connMode = "connect")}>
                Connect Account (recommended)
              </button>
              <button class="cell" class:on={connMode === "key"} onclick={() => (connMode = "key")}>
                Use Stream Key
              </button>
            </div>

            {#if connMode === "connect"}
              {#if connectedStatus}
                <div class="conn">
                  <span class="dot">●</span>
                  <span class="who">
                    <b>{connectedStatus.displayName || connectedStatus.login}</b>
                    <small>Stream key auto-filled · stream info editing enabled</small>
                  </span>
                  <button class="lnk" onclick={() => void disconnect()}>Disconnect</button>
                </div>
              {:else}
                <button class="btn connect" onclick={connect}>Connect {editingProvider.displayName} ▸</button>
              {/if}
              <p class="note">
                Connected accounts unlock the Go Live "Stream Information" panel (title / category / tags / thumbnail).
                Switch to <b>Use Stream Key</b> to paste a key manually — that still streams, just without metadata editing.
              </p>
            {:else}
              <div class="settings">
                {#key editingUuid + ":" + fService}
                  <PropertyForm kind="service" ref={editingUuid} />
                {/key}
              </div>
            {/if}
          {:else}
            <div class="settings">
              {#key editingUuid + ":" + fService}
                <PropertyForm kind="service" ref={editingUuid} />
              {/key}
            </div>
            {#if providers.length === 0}
              <p class="note">Account connection unavailable in this build.</p>
            {/if}
          {/if}
        </div>
      {/if}

      {#if formError}<p class="error">{formError}</p>{/if}

      <div class="actions">
        <button class="btn ghost" onclick={closeForm}>Close</button>
        <button class="btn primary" disabled={!formValid || saving} onclick={() => void save()}>
          {saving ? "Saving…" : editingUuid ? "Save" : "Create"}
        </button>
      </div>
    </div>
  {/if}
</div>

<style>
  .streams {
    padding: 8px 0 4px;
  }
  .grid {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
    gap: 12px;
  }
  .tile {
    display: flex;
    flex-direction: column;
    border: 1px solid var(--border);
    background: var(--bg-sunken);
  }
  .tile-body {
    display: flex;
    align-items: flex-start;
    gap: 12px;
    width: 100%;
    height: auto;
    padding: 12px;
    text-align: left;
    background: none;
    border: none;
    color: inherit;
    cursor: pointer;
  }
  .tile-body:hover {
    background: var(--bg-raised);
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
  .chip {
    font-size: 10px;
    padding: 1px 7px;
    border: 1px solid var(--border);
  }
  .chip.ok {
    color: var(--color-ok);
    border-color: var(--color-ok-bg);
  }
  .chip.key {
    color: var(--color-muted);
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
    justify-content: flex-end;
    padding: 8px 10px;
    border-top: 1px solid var(--border);
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
  .sect {
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
  .seg {
    display: flex;
    border: 1px solid var(--border);
    max-width: 360px;
    margin-bottom: 12px;
  }
  .seg .cell {
    flex: 1;
    text-align: center;
    padding: 7px;
    color: var(--text-dim);
    background: none;
    border: none;
    cursor: pointer;
    font: inherit;
    font-size: 12px;
  }
  .seg .cell:hover {
    color: var(--text);
  }
  .seg .cell.on {
    background: var(--accent);
    color: var(--color-accent-contrast);
    font-weight: 600;
  }
  .conn {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 12px;
    border: 1px solid var(--color-ok-bg);
    background: color-mix(in srgb, var(--color-ok) 6%, transparent);
    max-width: 360px;
  }
  .conn .dot {
    color: var(--color-ok);
  }
  .conn .who {
    flex: 1;
    min-width: 0;
  }
  .conn .who b {
    font-weight: 600;
  }
  .conn .who small {
    display: block;
    color: var(--text-dim);
    font-size: 11px;
  }
  .lnk {
    color: var(--accent);
    background: none;
    border: none;
    cursor: pointer;
    font: inherit;
    font-size: 12px;
    padding: 0;
  }
  .lnk:hover {
    text-decoration: underline;
  }
  .note {
    font-size: 11px;
    color: var(--color-muted);
    margin: 8px 0 0;
    max-width: 360px;
  }
  .settings {
    margin-top: 4px;
  }
  .btn.connect {
    background: var(--accent);
    border-color: var(--accent);
    color: var(--color-accent-contrast);
    font-weight: 600;
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
