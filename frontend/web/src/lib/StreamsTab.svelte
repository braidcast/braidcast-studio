<script lang="ts">
  import {
    obs,
    oauthAccounts,
    oauthLinkAccount,
    oauthDisconnect,
    type StreamProfileInfo,
    type ServiceType,
    type StreamProfileCreateParams,
    type StreamProfileUpdateParams,
    type OAuthProvider,
    type OAuthStatus,
    type OAuthAccountRow,
    type ListProperty,
  } from "./bridge";
  import PropertyForm from "./properties/PropertyForm.svelte";
  import { streamProfileStore } from "./streamProfileStore.svelte";
  import { oauthStore } from "./oauthStore.svelte";
  import { openOAuthConnect } from "./oauthConnectOpener.svelte";
  import CollectionDialog, { type DialogSpec } from "./CollectionDialog.svelte";
  import Icon from "./dock/Icon.svelte";
  import Segmented from "./Segmented.svelte";
  import EmptyState from "./EmptyState.svelte";
  import ToggleSwitch from "./ToggleSwitch.svelte";
  import Avatar from "./Avatar.svelte";
  import SplitPane from "./SplitPane.svelte";
  import { createReorder } from "./listReorder.svelte";

  let profiles = $derived(streamProfileStore.profiles);

  // Pointer drag-to-reorder the profile list. Optimistically reorders the shared store
  // array (avoids flicker), then persists via streamProfile.reorder; the backend's
  // streamProfile.changed emit + store refresh reconciles, so a failed call is non-fatal.
  const reorder = createReorder({
    getIds: () => streamProfileStore.profiles.map((p) => p.uuid),
    commit: async (order) => {
      const by = new Map(streamProfileStore.profiles.map((p) => [p.uuid, p]));
      streamProfileStore.profiles = order.map((id) => by.get(id)).filter((p): p is StreamProfileInfo => !!p);
      try {
        await obs.call("streamProfile.reorder", { order });
      } catch {
        // Reconciled by the streamProfile.changed emit + store refresh.
      }
    },
  });
  const dragRow = reorder.row;
  let serviceTypes = $state<ServiceType[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  // Platform OAuth: the providers that support account connection (empty in a build
  // without a client id) + the per-profile link status. Drives the dual-path
  // Connection section and the tile "linked" chips.
  let providers = $derived(oauthStore.providers);
  let statuses = $derived(oauthStore.statuses);

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
  // Add-flow auth default: a freshly-added profile opens its AUTHENTICATION toggle
  // on "Connect Account" once its OAuth provider resolves (service supports OAuth).
  // Editing an existing profile keeps openEdit's conservative default so a key-based
  // profile is never yanked off its key. `authDefaultApplied` fires the flip once,
  // so a manual switch back to "key" sticks.
  let justAdded = $state(false);
  let authDefaultApplied = $state(false);
  // Confirm dialog (destructive profile removal).
  let dialog = $state<DialogSpec | null>(null);
  // Row-scoped in-flight guard for Set-primary/Remove, keyed by profile uuid so a
  // double-click can't fire either action twice; both buttons on that row disable
  // together since neither should race the other.
  let busyProfileUuid = $state<string | null>(null);

  // The three connection types the top-level segmented splits into. The split is
  // fixed; the labels come from serviceTypes (live) so a build that renames them
  // stays in sync, and an absent id drops its option gracefully.
  const RTMP_COMMON = "rtmp_common";
  const RTMP_CUSTOM = "rtmp_custom";
  const WHIP_CUSTOM = "whip_custom";
  const CONN_TYPE_IDS = [RTMP_COMMON, RTMP_CUSTOM, WHIP_CUSTOM];

  // The promoted platform picker (rtmp_common only): the backend service object's
  // `service` list property, surfaced ABOVE the auth method instead of buried in
  // the stream-key form. Null when the type has no platform list or none loaded.
  let serviceProp = $state<ListProperty | null>(null);
  let serviceValue = $state("");
  // "Show all services" (rtmp_common's `show_all` bool): rebuilds the `service`
  // list between the popular subset and every known service. Surfaced beside the
  // promoted picker instead of buried in the key-path form.
  let showAll = $state(false);

  // Profiles + OAuth status/providers come from the shared stores (one source of
  // truth); these thin wrappers keep the existing post-mutation call sites, which
  // await a refresh to sequence the form snapshot below.
  const loadProfiles = (): Promise<void> => streamProfileStore.refresh();
  const refreshStatus = (): Promise<void> => oauthStore.refresh();

  async function loadAll() {
    error = null;
    try {
      serviceTypes = await obs.call("serviceTypes.list");
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    streamProfileStore.start();
    void loadAll();
  });

  // Subscribe to the shared OAuth store for this tab's lifetime (ref-counted).
  $effect(() => oauthStore.subscribe());

  // Keep the open form's profile snapshot fresh so its derived provider/platform
  // re-resolves after a save or an external change (was a side-effect of the old
  // loadProfiles reload).
  $effect(() => {
    const list = streamProfileStore.profiles;
    if (editingUuid) {
      const found = list.find((p) => p.uuid === editingUuid);
      if (found && found !== editingProfile) {
        editingProfile = found;
      }
    }
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

  // Status rows are keyed by accountId now (one row per connected account, shared
  // by every profile that reuses it). A profile resolves its state by matching its
  // own accountId; an empty accountId means "not linked".
  function connectedStatusFor(p: StreamProfileInfo): OAuthStatus | null {
    return oauthStore.connectedStatusForAccount(p.accountId);
  }

  // A token whose scopes are stale: reports connected:false but needsReconnect:true.
  // Distinct from "never linked" (no accountId / no row) so the UI can prompt a relink.
  function needsReconnectFor(p: StreamProfileInfo): OAuthStatus | null {
    if (!p.accountId) {
      return null;
    }
    return statuses.find((s) => s.accountId === p.accountId && s.needsReconnect && !s.connected) ?? null;
  }

  // The provider + connection state for the profile in the open edit form.
  const editingProvider = $derived(editingProfile ? providerForProfile(editingProfile) : null);
  const connectedStatus = $derived(editingProfile ? connectedStatusFor(editingProfile) : null);
  const needsReconnectStatus = $derived(editingProfile ? needsReconnectFor(editingProfile) : null);

  // Reuse picker (10e): a provider's already-connected accounts, offered on the
  // Connect-account path when this profile has no link yet. Selecting one links it
  // without a fresh grant; "Connect a different account" falls back to oauth.connect.
  let existing = $state<OAuthAccountRow[]>([]);

  async function loadExisting(providerId: string) {
    try {
      existing = await oauthAccounts(providerId);
    } catch {
      existing = [];
    }
  }

  // Keep the reuse list in sync with the selected provider + connection set. Reading
  // `statuses` makes a fresh connect/disconnect (elsewhere) re-populate the list.
  $effect(() => {
    const prov = editingProvider;
    void statuses;
    if (prov && connMode === "connect") {
      void loadExisting(prov.id);
    } else {
      existing = [];
    }
  });

  async function reuse(accountId: string) {
    if (!editingUuid) {
      return;
    }
    formError = null;
    try {
      await oauthLinkAccount(editingUuid, accountId);
      await Promise.all([loadProfiles(), refreshStatus()]);
    } catch (e) {
      formError = (e as Error).message;
    }
  }

  // The detail pane's title: while editing, prefer the live label field, else the
  // saved display name; the add form has no target yet.
  const formTitle = $derived(
    editingUuid
      ? fLabel.trim() || (editingProfile ? displayName(editingProfile) : "Stream Profile")
      : "New Stream Profile",
  );

  // Top-level connection-type options: the three fixed ids, labelled from live
  // serviceTypes; any id missing from this build is silently dropped.
  const connTypeOptions = $derived(
    CONN_TYPE_IDS.map((id) => serviceTypes.find((s) => s.id === id))
      .filter((s): s is ServiceType => !!s)
      .map((s) => ({ label: s.name, value: s.id })),
  );

  // Platforms Braidcast can OAuth-connect, listed under "Connectable accounts" in the
  // promoted Service picker. The live `providers` list (oauth.providers) is the source
  // of truth, but a build can omit a platform's client id — e.g. TwitchConfigured()==false —
  // so that platform's provider is absent from oauth.providers even though the platform
  // itself is OAuth-capable (and its service item, like "Kick", is common:true). This
  // static allowlist bridges that gap so Twitch/YouTube/Kick always group as connectable,
  // regardless of which providers this build actually registered. Add a platform => one
  // entry. Purely a grouping hint: the Connect button still requires a real registered
  // provider (providerForProfile), so this never fakes a link.
  const CONNECTABLE_PLATFORM_KEYS = ["twitch", "youtube", "kick"];

  // Lower-cased name prefixes that mark a service item as connectable: the static
  // allowlist unioned with every live provider's id + displayName, so a newly
  // registered provider joins automatically with no code change here.
  const connectablePrefixes = $derived.by(() => {
    const set = new Set<string>(CONNECTABLE_PLATFORM_KEYS);
    for (const p of providers) {
      set.add(p.id.trim().toLowerCase());
      set.add(p.displayName.trim().toLowerCase());
    }
    set.delete("");
    return set;
  });

  // Does a promoted-picker item name start with any connectable-platform prefix?
  // Case-insensitive prefix match, so both "YouTube - RTMPS" and "YouTube - HLS" match
  // "youtube", and "Kick" matches "kick", without a data edit on either side.
  function itemIsConnectable(itemName: string, prefixes: Set<string>): boolean {
    const n = itemName.trim().toLowerCase();
    for (const key of prefixes) {
      if (n === key || n.startsWith(key)) {
        return true;
      }
    }
    return false;
  }

  // Float OAuth-connectable platforms (Twitch/Kick/YouTube) to the top of the
  // promoted Service picker instead of leaving them buried among ~15 common
  // services. Falls back to the flat, unmodified list when none of the items match.
  const serviceGroups = $derived.by(() => {
    const items = serviceProp?.items ?? [];
    const prefixes = connectablePrefixes;
    const connectable = items.filter((it) => itemIsConnectable(it.name ?? String(it.value), prefixes));
    if (connectable.length === 0) {
      return { connectable: [] as typeof items, rest: items };
    }
    const connectableValues = new Set(connectable.map((it) => String(it.value)));
    return { connectable, rest: items.filter((it) => !connectableValues.has(String(it.value))) };
  });

  // Prefer "Streaming Services" (rtmp_common) as the new-profile default — most users
  // pick a platform, not a custom RTMP URL. serviceTypes are sorted by display name,
  // so match by id (then a "stream"-named type) rather than trusting index 0.
  function defaultServiceId(): string {
    return (
      serviceTypes.find((s) => s.id === "rtmp_common")?.id ??
      serviceTypes.find((s) => s.name.toLowerCase().includes("stream"))?.id ??
      serviceTypes[0]?.id ??
      "rtmp_common"
    );
  }

  function openAdd() {
    editingUuid = null;
    editingProfile = null;
    fLabel = "";
    fService = defaultServiceId();
    connMode = "key";
    justAdded = true;
    authDefaultApplied = false;
    formError = null;
    formOpen = true;
  }

  function openEdit(p: StreamProfileInfo) {
    editingUuid = p.uuid;
    editingProfile = p;
    fLabel = p.label;
    fService = p.service || serviceTypes[0]?.id || "rtmp_common";
    // Default to Connect when an account is linked OR needs a relink (so the warn
    // panel shows); otherwise (incl. no provider) start on the stream-key path so an
    // existing key-based profile is never yanked off its key.
    connMode = providerForProfile(p) && (connectedStatusFor(p) || needsReconnectFor(p)) ? "connect" : "key";
    justAdded = false;
    authDefaultApplied = true;
    formError = null;
    formOpen = true;
  }

  // Add flow only: when a freshly-added profile's service resolves to an OAuth
  // provider and it isn't linked yet, default the AUTHENTICATION toggle to
  // "Connect Account". Fires once (authDefaultApplied) so a manual switch to "key"
  // is respected. A key-only service (no provider) leaves the "Use Stream Key"
  // default untouched.
  $effect(() => {
    if (!formOpen || !justAdded || authDefaultApplied) {
      return;
    }
    if (editingProvider && !connectedStatus && !needsReconnectStatus) {
      connMode = "connect";
      authDefaultApplied = true;
    }
  });

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
    const accountId = editingProfile?.accountId;
    if (!accountId) {
      return;
    }
    formError = null;
    try {
      const res = await oauthDisconnect(accountId);
      if ("needsConfirm" in res) {
        // The account is reused by several profiles; confirm before unlinking all.
        const names = res.profiles.map((p) => p.name).join(", ");
        dialog = {
          kind: "confirm",
          title: "Disconnect Account",
          message: `This account is linked to ${res.profiles.length} profiles (${names}). Disconnecting removes it and unlinks all of them.`,
          confirmLabel: "Disconnect All",
          onCommit: () => void forceDisconnect(accountId),
        };
        return;
      }
      await Promise.all([loadProfiles(), refreshStatus()]);
    } catch (e) {
      formError = (e as Error).message;
    }
  }

  async function forceDisconnect(accountId: string) {
    formError = null;
    try {
      await oauthDisconnect(accountId, true);
      await Promise.all([loadProfiles(), refreshStatus()]);
    } catch (e) {
      formError = (e as Error).message;
    }
  }

  // Load the promoted platform list for the current edit target (rtmp_common only).
  async function loadServiceOptions(uuid: string) {
    try {
      const r = await obs.call("properties.get", { kind: "service", ref: uuid });
      const d = r.props.find((p) => p.name === "service");
      if (d && d.type === "list") {
        serviceProp = d;
        serviceValue = String(r.values.service ?? d.value ?? "");
      } else {
        serviceProp = null;
      }
      const sa = r.props.find((p) => p.name === "show_all");
      showAll = Boolean(r.values.show_all ?? (sa && sa.type === "bool" ? sa.value : false));
    } catch {
      // Non-fatal: leave the promoted picker hidden; the key-path form still lists it.
      serviceProp = null;
      showAll = false;
    }
  }

  // Keep the promoted platform picker in sync with the edit target and its
  // connection type. Only rtmp_common exposes a platform list.
  $effect(() => {
    const uuid = editingUuid;
    if (uuid && fService === RTMP_COMMON) {
      void loadServiceOptions(uuid);
    } else {
      serviceProp = null;
      showAll = false;
    }
  });

  // 10d fix: commit the connection-type change immediately in edit mode so the
  // backend service is re-pointed and the fields below re-fetch. Add mode just
  // tracks it locally — Create already sends `service`.
  async function changeConnType(newId: string) {
    if (newId === fService) return;
    if (!editingUuid) {
      fService = newId;
      return;
    }
    saving = true;
    formError = null;
    try {
      const updated = await obs.call("streamProfile.update", {
        uuid: editingUuid,
        label: fLabel.trim(),
        service: newId,
      });
      fService = updated.service;
      editingProfile = updated;
      await loadProfiles();
    } catch (e) {
      formError = (e as Error).message;
    } finally {
      saving = false;
    }
  }

  // 10c fix: push the promoted platform choice onto the backend service, then
  // refresh so the provider derivation (Connect Account) and the key-path fields
  // follow the new platform.
  async function changeService(value: string) {
    if (!editingUuid || value === serviceValue) return;
    serviceValue = value;
    formError = null;
    try {
      const r = await obs.call("properties.set", {
        kind: "service",
        ref: editingUuid,
        settings: { service: value },
      });
      const d = r.props.find((p) => p.name === "service");
      if (d && d.type === "list") {
        serviceProp = d;
        serviceValue = String(r.values.service ?? value);
      }
      await loadProfiles();
    } catch (e) {
      formError = (e as Error).message;
    }
  }

  // Toggling "Show all services" rebuilds the backend's `service` list (popular
  // subset <-> everything) via its modified callback; refresh the promoted picker
  // + selection from the response exactly like changeService does.
  async function changeShowAll(value: boolean) {
    if (!editingUuid) return;
    showAll = value;
    formError = null;
    try {
      const r = await obs.call("properties.set", {
        kind: "service",
        ref: editingUuid,
        settings: { show_all: value },
      });
      const d = r.props.find((p) => p.name === "service");
      if (d && d.type === "list") {
        serviceProp = d;
        serviceValue = String(r.values.service ?? serviceValue);
      }
      const sa = r.props.find((p) => p.name === "show_all");
      showAll = Boolean(r.values.show_all ?? (sa && sa.type === "bool" ? sa.value : value));
      await loadProfiles();
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
    if (busyProfileUuid === p.uuid) return;
    busyProfileUuid = p.uuid;
    try {
      await obs.call("streamProfile.remove", { uuid: p.uuid });
      if (editingUuid === p.uuid) formOpen = false;
      await loadProfiles();
    } catch (e) {
      error = (e as Error).message;
    } finally {
      busyProfileUuid = null;
    }
  }

  async function setPrimary(p: StreamProfileInfo) {
    if (p.isPrimary || busyProfileUuid === p.uuid) return;
    busyProfileUuid = p.uuid;
    try {
      await obs.call("streamProfile.setPrimary", { uuid: p.uuid });
      await loadProfiles();
    } catch (e) {
      error = (e as Error).message;
    } finally {
      busyProfileUuid = null;
    }
  }
</script>

<div class="streams">
  <SplitPane storageKey="braidcast.split.streams" default={264} left={masterList} right={detailPane} />
</div>

{#snippet masterList()}
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
        {#each profiles as p, i (p.uuid)}
          {@const linked = connectedStatusFor(p)}
          {@const reconnect = needsReconnectFor(p)}
          {@const acctName = linked ? linked.displayName || linked.login : ""}
          {#if reorder.dragging && reorder.dropIndex === i}<div class="reorder-line"></div>{/if}
          <button
            class="nav-item"
            class:active={formOpen && editingUuid === p.uuid}
            class:lifting={reorder.dragIndex === i}
            use:dragRow={i}
            onclick={() => {
              if (reorder.consumeClick()) return;
              openEdit(p);
            }}
          >
            <span class="nav-av">
              <Avatar url={linked?.avatarUrl} name={acctName || displayName(p)} size={36} />
            </span>
            <span class="nav-text">
              <span class="nav-name">
                {#if p.isPrimary}<span class="star" title="Primary"><Icon name="star-filled" size={11} /></span>{/if}
                <span class="nav-label">{displayName(p)}</span>
              </span>
              <span class="nav-sub">
                {#if acctName}
                  <span class="acct-name">{acctName}</span>
                  <span class="sep">|</span>
                {/if}
                <span class="nav-plat">{p.serviceLabel}</span>
              </span>
            </span>
            <span class="nav-status">
              {#if linked}
                <span class="conn-dot" title="Connected"></span>
              {:else if reconnect}
                <span class="chip warn">⟳ Reconnect</span>
              {/if}
            </span>
          </button>
        {/each}
        {#if reorder.dragging && reorder.dropIndex === profiles.length}<div class="reorder-line"></div>{/if}
      {/if}
      {#if loaded}
        <button class="add-btn" onclick={openAdd}><Icon name="plus" size={13} /> Add Stream Profile</button>
      {/if}
    </div>
  </div>
{/snippet}

{#snippet detailPane()}
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
              <button
                class="mini"
                title="Set as primary"
                disabled={busyProfileUuid === editingProfile.uuid}
                onclick={() => editingProfile && void setPrimary(editingProfile)}
              >
                <span class="star"><Icon name="star" size={12} /></span> Primary
              </button>
            {/if}
            <button
              class="mini danger"
              title="Remove"
              disabled={busyProfileUuid === editingProfile.uuid}
              onclick={() => editingProfile && confirmRemove(editingProfile)}
            >
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
          <Segmented options={connTypeOptions} value={fService} onChange={(v) => void changeConnType(v)} size="md" />
          <span class="fhint">How this profile reaches its destination.</span>
        </div>

        {#if editingUuid}
          {#if fService === RTMP_COMMON}
            {#if serviceProp}
              <div class="field">
                <span class="flabel">Service</span>
                <select value={serviceValue} onchange={(e) => void changeService(e.currentTarget.value)}>
                  {#if serviceGroups.connectable.length > 0}
                    <optgroup label="Connectable accounts">
                      {#each serviceGroups.connectable as it (String(it.value))}
                        <option value={String(it.value)} disabled={it.disabled}>{it.name ?? String(it.value)}</option>
                      {/each}
                    </optgroup>
                    <optgroup label="All services">
                      {#each serviceGroups.rest as it (String(it.value))}
                        <option value={String(it.value)} disabled={it.disabled}>{it.name ?? String(it.value)}</option>
                      {/each}
                    </optgroup>
                  {:else}
                    {#each serviceGroups.rest as it (String(it.value))}
                      <option value={String(it.value)} disabled={it.disabled}>{it.name ?? String(it.value)}</option>
                    {/each}
                  {/if}
                </select>
                <div class="show-all">
                  <ToggleSwitch size="sm" checked={showAll} onchange={(v) => void changeShowAll(v)} />
                  <span class="fhint show-all-txt">Show all services</span>
                </div>
              </div>
            {/if}

            <div class="sect">
              <div class="exp-head">Authentication</div>

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
                      <Avatar
                        url={connectedStatus.avatarUrl}
                        name={connectedStatus.displayName || connectedStatus.login}
                        size={26}
                      />
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
                  {:else if existing.length > 0}
                    <div class="reuse">
                      <div class="reuse-label">Reuse existing account</div>
                      {#each existing as a (a.accountId)}
                        <button class="reuse-item" onclick={() => void reuse(a.accountId)}>
                          <span class="reuse-name">{a.displayName || a.login}</span>
                          {#if a.needsReconnect}<span class="reuse-flag">reconnect</span>{/if}
                        </button>
                      {/each}
                    </div>
                    <button class="btn connect" onclick={connect}>
                      Connect a different account <Icon name="caret-right" size={12} />
                    </button>
                  {:else}
                    <button class="btn connect" onclick={connect}>
                      Connect {editingProvider.displayName} <Icon name="caret-right" size={12} />
                    </button>
                  {/if}
                  <p class="note">
                    Connected accounts unlock the Go Live "Stream Information" panel (title / category / tags /
                    thumbnail). Switch to <b>Use Stream Key</b> to paste a key manually — that still streams, just without
                    metadata editing.
                  </p>
                {:else}
                  <div class="settings">
                    {#key editingUuid + ":" + fService + ":" + serviceValue}
                      <PropertyForm kind="service" ref={editingUuid} exclude={["service", "show_all"]} />
                    {/key}
                  </div>
                {/if}
              {:else}
                <div class="settings">
                  {#key editingUuid + ":" + fService + ":" + serviceValue}
                    <PropertyForm kind="service" ref={editingUuid} exclude={["service", "show_all"]} />
                  {/key}
                </div>
                {#if providers.length === 0}
                  <p class="note">Account connection unavailable in this build.</p>
                {/if}
              {/if}
            </div>
          {:else}
            <div class="sect">
              <div class="exp-head">{fService === WHIP_CUSTOM ? "WHIP endpoint" : "Server"}</div>
              <div class="settings">
                {#key editingUuid + ":" + fService}
                  <PropertyForm kind="service" ref={editingUuid} />
                {/key}
              </div>
            </div>
          {/if}
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
{/snippet}

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
    flex: 1 1 auto;
    min-width: 0;
    display: flex;
    flex-direction: column;
    min-height: 0;
    border-right: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .reorder-line {
    height: 2px;
    margin: 0 8px;
    background: var(--color-accent);
  }
  .nav-item.lifting {
    opacity: 0.4;
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
    display: grid;
    grid-template-columns: auto minmax(0, 1fr) auto;
    align-items: center;
    gap: 11px;
    width: 100%;
    height: auto;
    text-align: left;
    padding: 9px 11px;
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
  .nav-av {
    display: inline-flex;
    flex: 0 0 auto;
  }
  .nav-text {
    display: flex;
    flex-direction: column;
    gap: 4px;
    min-width: 0;
  }
  .nav-status {
    display: inline-flex;
    align-items: center;
    flex: 0 0 auto;
  }
  .conn-dot {
    width: 8px;
    height: 8px;
    background: var(--color-ok);
    flex: 0 0 auto;
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
    gap: 5px;
    min-width: 0;
  }
  .nav-plat {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
    white-space: nowrap;
    flex: 0 0 auto;
  }
  .acct-name {
    flex: 0 1 auto;
    min-width: 0;
    font-family: var(--font-ui);
    font-size: 11px;
    color: var(--color-dim);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .sep {
    flex: 0 0 auto;
    color: var(--color-muted);
    font-family: var(--font-mono);
    font-size: 10px;
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
    width: 100%;
    height: auto;
    padding: 11px;
    border: var(--border-weight) dashed var(--color-border);
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
  optgroup {
    color: var(--color-muted);
  }
  option {
    color: var(--color-text);
  }
  .fhint {
    display: block;
    margin-top: 6px;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .show-all {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-top: 8px;
  }
  .show-all-txt {
    margin-top: 0;
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
  .reuse {
    display: flex;
    flex-direction: column;
    gap: 6px;
    margin-bottom: 8px;
  }
  .reuse-label {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: var(--color-muted);
    margin-bottom: 2px;
  }
  .reuse-item {
    display: flex;
    align-items: center;
    gap: 8px;
    width: 100%;
    text-align: left;
    padding: 9px 11px;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    cursor: pointer;
    font: inherit;
    font-size: 12px;
    transition:
      border-color 0.1s ease,
      background 0.1s ease;
  }
  .reuse-item:hover {
    border-color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 10%, transparent);
  }
  .reuse-name {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-weight: 600;
  }
  .reuse-flag {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.05em;
    text-transform: uppercase;
    color: var(--color-accent);
    border: var(--border-weight) solid var(--color-accent);
    padding: 1px 5px;
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
