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
  import CollectionDialog, { type DialogSpec } from "./CollectionDialog.svelte";
  import Icon from "./dock/Icon.svelte";
  import Segmented from "./Segmented.svelte";
  import EmptyState from "./EmptyState.svelte";

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
  // Confirm dialog (destructive profile removal).
  let dialog = $state<DialogSpec | null>(null);

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

  // Robust display name: a fresh profile can have an empty label AND empty platform
  // (platform is derived from the service and may lag), which collapsed the list row
  // to a blank line. Fall back label -> platform -> a literal so a row always names
  // itself.
  function displayName(p: StreamProfileInfo): string {
    return p.label?.trim() || p.platform?.trim() || "Untitled profile";
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

  // A token whose scopes are stale: reports connected:false but needsReconnect:true.
  // Distinct from "never linked" (no row / both false) so the UI can prompt a relink.
  function needsReconnectFor(uuid: string): OAuthStatus | null {
    return statuses.find((s) => s.profileUuid === uuid && s.needsReconnect && !s.connected) ?? null;
  }

  // The provider + connection state for the profile in the open edit form.
  const editingProvider = $derived(editingProfile ? providerForProfile(editingProfile) : null);
  const connectedStatus = $derived(editingUuid ? connectedStatusFor(editingUuid) : null);
  const needsReconnectStatus = $derived(editingUuid ? needsReconnectFor(editingUuid) : null);

  // The detail pane's title: while editing, prefer the live label field, else the
  // saved display name; the add form has no target yet.
  const formTitle = $derived(
    editingUuid
      ? fLabel.trim() || (editingProfile ? displayName(editingProfile) : "Stream Profile")
      : "New Stream Profile",
  );

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
    // Default to Connect when an account is linked OR needs a relink (so the warn
    // panel shows); otherwise (incl. no provider) start on the stream-key path.
    connMode =
      providerForProfile(p) && (connectedStatusFor(p.uuid) || needsReconnectFor(p.uuid)) ? "connect" : "key";
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

  function confirmRemove(p: StreamProfileInfo) {
    dialog = {
      kind: "confirm",
      title: "Remove Profile",
      message: `Remove profile "${displayName(p)}"? Its stream key will be forgotten.`,
      confirmLabel: "Remove",
      onCommit: () => void remove(p),
    };
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
  <div class="master">
    <div class="master-head">
      <span class="mh-title">Profiles</span>
      {#if loaded}<span class="mh-count">{profiles.length}</span>{/if}
    </div>
    <div class="master-list">
      {#if !loaded}
        <p class="dim pad">Loading stream profiles…</p>
      {:else if profiles.length === 0}
        <p class="dim pad">No stream profiles yet.</p>
      {:else}
        {#each profiles as p (p.uuid)}
          <button class="nav-item" class:active={formOpen && editingUuid === p.uuid} onclick={() => openEdit(p)}>
            <span class="nav-name">
              {#if p.isPrimary}<span class="star" title="Primary"><Icon name="star-filled" size={11} /></span>{/if}
              <span class="nav-label">{displayName(p)}</span>
            </span>
            <span class="nav-sub">
              <span class="nav-plat">{p.serviceLabel}</span>
              {#if providerForProfile(p)}
                {#if connectedStatusFor(p.uuid)}
                  <span class="chip ok">linked</span>
                {:else if needsReconnectFor(p.uuid)}
                  <span class="chip warn">reconnect</span>
                {/if}
              {/if}
            </span>
          </button>
        {/each}
      {/if}
    </div>
    <button class="add-btn" onclick={openAdd}><Icon name="plus" size={13} /> Add Stream Profile</button>
  </div>

  <div class="detail">
    {#if error}<p class="error">{error}</p>{/if}

    {#if formOpen}
      <div class="form">
        <div class="form-head">
          <div class="fh-titles">
            <span class="fh-name">{formTitle}</span>
            <span class="fh-sub">{editingUuid ? "Edit destination credential" : "Reusable destination credential"}</span>
          </div>
          <span class="fh-spacer"></span>
          {#if editingUuid && editingProfile}
            {#if !editingProfile.isPrimary}
              <button class="mini" title="Set as primary" onclick={() => editingProfile && void setPrimary(editingProfile)}>
                <span class="star"><Icon name="star" size={12} /></span> Primary
              </button>
            {/if}
            <button class="mini danger" title="Remove" onclick={() => editingProfile && confirmRemove(editingProfile)}>
              <svg
                width="13"
                height="13"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="1.7"
                stroke-linecap="round"
                stroke-linejoin="round"><path d="M4 7h16M9 7V5a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2M6 7l1 13h10l1-13" /></svg
              >
              Remove
            </button>
          {/if}
        </div>

        <div class="field">
          <span class="flabel">Label</span>
          <!-- svelte-ignore a11y_autofocus -->
          <input type="text" bind:value={fLabel} autofocus placeholder="e.g. Main Channel" />
        </div>

        <div class="field">
          <span class="flabel">Connection type</span>
          <select bind:value={fService}>
            {#each serviceTypes as s (s.id)}
              <option value={s.id}>{s.name}</option>
            {/each}
          </select>
          <span class="fhint">Pick the platform and server below.</span>
        </div>

        {#if editingUuid}
          <div class="sect">
            <div class="exp-head">Connection</div>

            {#if editingProvider}
              <div class="conn-seg">
                <Segmented
                  options={[
                    { label: "Connect Account", value: "connect" },
                    { label: "Use Stream Key", value: "key" },
                  ]}
                  value={connMode}
                  onChange={(v) => (connMode = v as "connect" | "key")}
                />
              </div>

              {#if connMode === "connect"}
                {#if connectedStatus}
                  <div class="conn">
                    <span class="dot"><Icon name="dot" size={10} /></span>
                    <span class="who">
                      <b>{connectedStatus.displayName || connectedStatus.login}</b>
                      <small>Stream key auto-filled · stream info editing enabled</small>
                    </span>
                    <button class="lnk" onclick={() => void disconnect()}>Disconnect</button>
                  </div>
                {:else if needsReconnectStatus}
                  <div class="conn warn">
                    <span class="dot warn"><Icon name="warn" size={13} /></span>
                    <span class="who">
                      <b>Reconnect needed</b>
                      <small>Your authorization is out of date — reconnect to keep editing stream info.</small>
                    </span>
                  </div>
                  <button class="btn connect" onclick={connect}>
                    Reconnect {editingProvider.displayName} <Icon name="caret-right" size={12} />
                  </button>
                {:else}
                  <button class="btn connect" onclick={connect}>
                    Connect {editingProvider.displayName} <Icon name="caret-right" size={12} />
                  </button>
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
                {#key editingUuid + ":" + (editingProfile?.service ?? "")}
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
          {#if !editingUuid}
            <button class="btn ghost" onclick={closeForm}>Cancel</button>
          {/if}
          <button class="btn primary" disabled={!formValid || saving} onclick={() => void save()}>
            {saving ? "Saving…" : editingUuid ? "Save" : "Create"}
          </button>
        </div>
      </div>
    {:else}
      <EmptyState title="No profile selected" sub="Select a stream profile to edit, or add a new one.">
        {#snippet icon()}
          <svg
            width="30"
            height="30"
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            stroke-width="1.3"
            stroke-linecap="round"
            stroke-linejoin="round"
            ><rect x="3" y="4" width="18" height="14" /><path d="M8 20h8M12 18v2M3 14h18" /></svg
          >
        {/snippet}
      </EmptyState>
    {/if}
  </div>
</div>

{#if dialog}
  <CollectionDialog {...dialog} onClose={() => (dialog = null)} />
{/if}

<style>
  .streams {
    flex: 1;
    display: flex;
    min-height: 0;
    min-width: 0;
  }

  /* ---- master (profile list) -------------------------------------------- */
  .master {
    flex: 0 0 264px;
    display: flex;
    flex-direction: column;
    min-height: 0;
    border-right: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .master-head {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 8px;
    height: 34px;
    padding: 0 14px;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .mh-title {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .mh-count {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-dim);
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    padding: 0 6px;
    line-height: 16px;
  }
  .master-list {
    flex: 1;
    min-height: 0;
    overflow: auto;
    padding: 10px 8px;
    display: flex;
    flex-direction: column;
    gap: 5px;
  }
  .nav-item {
    display: flex;
    flex-direction: column;
    gap: 6px;
    width: 100%;
    height: auto;
    text-align: left;
    padding: 10px 11px;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    border-left: 3px solid transparent;
    color: var(--color-text);
    cursor: pointer;
    transition:
      border-color 0.1s ease,
      background 0.1s ease;
  }
  .nav-item:hover {
    border-color: color-mix(in srgb, var(--color-accent) 45%, var(--color-border));
    background: var(--color-surface-2);
  }
  .nav-item.active {
    border-left-color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 10%, transparent);
  }
  .nav-name {
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
  }
  .nav-label {
    font-family: var(--font-ui);
    font-size: 13px;
    font-weight: 600;
    letter-spacing: -0.01em;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .nav-item.active .nav-label {
    color: var(--color-accent);
  }
  .star {
    display: inline-flex;
    align-items: center;
    color: var(--color-accent);
    flex: 0 0 auto;
    line-height: 1;
  }
  .nav-sub {
    display: flex;
    align-items: center;
    gap: 7px;
    min-width: 0;
  }
  .nav-plat {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .chip {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.05em;
    text-transform: uppercase;
    padding: 1px 5px;
    border: var(--border-weight) solid var(--color-border);
  }
  .chip.ok {
    color: var(--color-ok);
    border-color: color-mix(in srgb, var(--color-ok) 45%, transparent);
  }
  .chip.warn {
    color: var(--color-accent);
    border-color: var(--color-accent);
  }
  .add-btn {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    height: auto;
    padding: 12px;
    border: 0;
    border-top: var(--border-weight) solid var(--color-border);
    background: transparent;
    color: var(--color-dim);
    font-family: var(--font-ui);
    font-size: 12px;
    cursor: pointer;
    transition: color 0.1s ease;
  }
  .add-btn:hover {
    color: var(--color-accent);
    border-color: var(--color-border);
  }

  /* ---- detail (editor pane) --------------------------------------------- */
  .detail {
    flex: 1;
    min-width: 0;
    overflow: auto;
    padding: 24px 28px 34px;
  }
  .form {
    max-width: 480px;
  }
  .form-head {
    display: flex;
    align-items: flex-start;
    gap: 8px;
    padding-bottom: 16px;
    margin-bottom: 18px;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .fh-titles {
    display: flex;
    flex-direction: column;
    gap: 3px;
    min-width: 0;
  }
  .fh-name {
    font-family: var(--font-ui);
    font-size: 16px;
    font-weight: 600;
    letter-spacing: -0.01em;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .fh-sub {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.04em;
    color: var(--color-muted);
  }
  .fh-spacer {
    flex: 1;
  }
  .mini {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    height: auto;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    cursor: pointer;
    font: inherit;
    font-size: 12px;
    padding: 6px 10px;
    line-height: 1;
    white-space: nowrap;
  }
  .mini:hover:not(:disabled) {
    color: var(--color-text);
    border-color: var(--color-muted);
  }
  .mini.danger:hover:not(:disabled) {
    color: var(--color-live);
    border-color: var(--color-live);
  }

  .field {
    margin-bottom: 14px;
    max-width: 380px;
  }
  .flabel {
    display: block;
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: var(--color-muted);
    margin-bottom: 6px;
  }
  input,
  select {
    width: 100%;
  }
  .fhint {
    display: block;
    margin-top: 6px;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }

  .sect {
    margin-top: 20px;
    padding-top: 18px;
    border-top: var(--border-weight) solid var(--color-border);
    max-width: 440px;
  }
  .exp-head {
    font-family: var(--font-mono);
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: var(--color-dim);
    margin-bottom: 12px;
  }
  .conn-seg {
    margin-bottom: 14px;
  }
  .conn {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 11px 13px;
    border: var(--border-weight) solid color-mix(in srgb, var(--color-ok) 45%, transparent);
    background: color-mix(in srgb, var(--color-ok) 7%, transparent);
  }
  .conn .dot {
    display: inline-flex;
    align-items: center;
    flex: 0 0 auto;
    color: var(--color-ok);
  }
  .conn.warn {
    border-color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 8%, transparent);
  }
  .conn.warn .dot {
    color: var(--color-accent);
  }
  .conn .who {
    flex: 1;
    min-width: 0;
  }
  .conn .who b {
    font-weight: 600;
    color: var(--color-text);
  }
  .conn .who small {
    display: block;
    margin-top: 2px;
    color: var(--color-muted);
    font-size: 11px;
  }
  .lnk {
    height: auto;
    color: var(--color-accent);
    background: none;
    border: none;
    cursor: pointer;
    font: inherit;
    font-size: 12px;
    padding: 0;
    flex: 0 0 auto;
  }
  .lnk:hover {
    text-decoration: underline;
  }
  .note {
    font-size: 11px;
    line-height: 1.55;
    color: var(--color-muted);
    margin: 10px 0 0;
  }
  .note b {
    color: var(--color-dim);
    font-weight: 600;
  }
  .settings {
    margin-top: 4px;
  }

  .actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
    margin-top: 20px;
    max-width: 480px;
  }
  .btn {
    height: auto;
    padding: 8px 16px;
    font: inherit;
    font-size: 12px;
    cursor: pointer;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
    color: var(--color-dim);
  }
  .btn:hover:not(:disabled) {
    color: var(--color-text);
    border-color: var(--color-muted);
  }
  .btn.connect,
  .btn.primary {
    background: var(--color-accent);
    border-color: var(--color-accent);
    color: var(--color-accent-ink);
    font-weight: 600;
  }
  .btn.connect {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
  }
  .btn.connect:hover:not(:disabled),
  .btn.primary:hover:not(:disabled) {
    color: var(--color-accent-ink);
    background: color-mix(in srgb, var(--color-accent) 88%, var(--color-text));
  }
  .btn.primary:disabled {
    opacity: 0.45;
    cursor: default;
  }
  .btn.ghost {
    background: none;
  }
  .dim {
    color: var(--color-muted);
    margin: 0;
  }
  .pad {
    padding: 12px;
  }
  .error {
    color: var(--color-live);
    margin: 0 0 8px;
    font-size: 12px;
  }
</style>
