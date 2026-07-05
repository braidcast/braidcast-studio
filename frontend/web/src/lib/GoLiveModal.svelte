<script lang="ts">
  import {
    obs,
    type OAuthProvider,
    type OAuthProviderField,
    type OAuthStatus,
    type OutputBindingInfo,
    type StreamProfileInfo,
  } from "./bridge";
  import { goLiveModal, closeGoLiveModal } from "./goLiveModalOpener.svelte";
  import { outputBindingStore } from "./outputBindingStore.svelte";
  import { streamProfileStore } from "./streamProfileStore.svelte";
  import { oauthStore } from "./oauthStore.svelte";
  import { showToast } from "./toastStore.svelte";
  import GoLiveFieldInput from "./GoLiveFieldInput.svelte";
  import Modal from "./Modal.svelte";
  import Segmented from "./Segmented.svelte";

  const VIEW_OPTIONS = [
    { label: "Simple", value: "simple" },
    { label: "Advanced", value: "advanced" },
  ];

  // The modal renders ENTIRELY from oauth.providers capability descriptors. Field
  // dispatch lives in GoLiveFieldInput, keyed by descriptor `type` (text/textarea/
  // tags/category/image/enum/bool/labelset) — never by platform id — so a new
  // provider (or a new field type) renders with zero changes here.

  // One stream feeding a channel: a distinct stream profile bound via an enabled
  // output binding. Several streams (e.g. a 16:9 + a 9:16 profile) can post to the
  // same channel; `canvasName` carries which canvas armed this stream.
  interface Stream {
    profileUuid: string;
    profile: StreamProfileInfo;
    label: string;
    canvasName: string;
  }

  // One channel = one account identity (`accountId` = "providerId:userId"). Profiles
  // that reuse the same account collapse into this single card; `streams` lists them.
  // `connected` (provider resolved + OAuth linked) gates the editable card.
  interface Channel {
    accountId: string;
    provider: OAuthProvider | null;
    status: OAuthStatus | undefined;
    login: string;
    connected: boolean;
    // Token scopes are stale: the backend refuses streamMeta, so this channel is
    // connected:false — excluded from the push. It still goes live via key.
    needsReconnect: boolean;
    streams: Stream[];
  }

  // Provider/status/binding/profile lists come from the shared stores (one source of
  // truth); `loaded` gates the modal until they + the live flag have settled.
  let providers = $derived(oauthStore.providers);
  let statuses = $derived(oauthStore.statuses);
  let bindings = $derived(outputBindingStore.bindings);
  let profiles = $derived(streamProfileStore.profiles);
  let loaded = $state(false);
  let isLive = $state(false);
  let submitting = $state(false);
  let view = $state<"simple" | "advanced">("simple");
  // Persist this dialog's values to the remembered store on confirm so the next
  // go-live prefills from them. Default ON — most valuable for YouTube, whose live
  // metadata comes back empty.
  let remember = $state(true);

  // Three inheritance layers, resolved shared -> channel -> stream by
  // effectiveFields (later wins, empties omitted). `sharedValues` holds only
  // shareable keys (mock "Shared defaults" block), driven by `sharedFields`, so
  // any shareable field — not just title/tags — gets a shared source.
  let sharedValues = $state<Record<string, unknown>>({});
  // Per-channel defaults, keyed by accountId then field key. An empty shareable
  // field inherits the shared value; a non-shareable field stands alone. Applied
  // to every stream in the channel unless the stream overrides it.
  let channelValues = $state<Record<string, Record<string, unknown>>>({});
  // Per-stream overrides, keyed by profileUuid. A filled key diverges that single
  // broadcast from its channel default; empty keys inherit the channel. Advanced
  // mode only — never written in Simple mode.
  let streamOverrides = $state<Record<string, Record<string, unknown>>>({});
  // Advanced-only UI state: which streams have their override field set expanded.
  // Toggling off clears that stream's overrides so it cleanly inherits again.
  let streamOverrideOn = $state<Record<string, boolean>>({});

  function setField(id: string, key: string, val: unknown): void {
    channelValues[id] = { ...(channelValues[id] ?? {}), [key]: val };
  }
  function getVal(id: string, key: string): unknown {
    return channelValues[id]?.[key];
  }
  function setStreamField(uuid: string, key: string, val: unknown): void {
    streamOverrides[uuid] = { ...(streamOverrides[uuid] ?? {}), [key]: val };
  }
  function getStreamVal(uuid: string, key: string): unknown {
    return streamOverrides[uuid]?.[key];
  }
  function toggleStreamOverride(uuid: string): void {
    const on = !streamOverrideOn[uuid];
    streamOverrideOn[uuid] = on;
    if (!on) {
      delete streamOverrides[uuid];
      streamOverrides = { ...streamOverrides };
    }
  }
  function streamOverrideCount(uuid: string, p: OAuthProvider): number {
    const ov = streamOverrides[uuid] ?? {};
    return p.fields.filter((f) => !isEmptyVal(f.type, ov[f.key])).length;
  }

  // "Empty" per descriptor type — the inheritance/omission predicate. A bool that
  // has been set (even to false) counts as present; everything else is empty when
  // blank/missing.
  function isEmptyVal(type: string, v: unknown): boolean {
    switch (type) {
      case "tags":
      case "labelset":
        return !Array.isArray(v) || v.length === 0;
      case "category":
        return v == null;
      case "bool":
        return v === undefined || v === null;
      default:
        return typeof v !== "string" || v.trim() === "";
    }
  }

  // Human-readable shared value for a field's inherit ghost (any shareable key).
  function sharedGhostText(f: OAuthProviderField): string {
    const v = sharedValues[f.key];
    if (f.type === "tags" || f.type === "labelset") {
      return Array.isArray(v) ? v.join(", ") : "";
    }
    if (f.type === "category") {
      return v && typeof v === "object" ? ((v as { name?: string }).name ?? "") : "";
    }
    return typeof v === "string" ? v : "";
  }

  // Field grouping (data lists, not branches): simple shareable render as overrides
  // (ghost/amber), simple non-shareable render normally, advanced go under the
  // dashed "<Platform>-only" divider.
  function simpleShareable(p: OAuthProvider): OAuthProviderField[] {
    return p.fields.filter((f) => f.tier !== "advanced" && f.shareable);
  }
  function simpleNonShareable(p: OAuthProvider): OAuthProviderField[] {
    return p.fields.filter((f) => f.tier !== "advanced" && !f.shareable);
  }
  function advancedFields(p: OAuthProvider): OAuthProviderField[] {
    return p.fields.filter((f) => f.tier === "advanced");
  }

  function isOverridden(id: string, f: OAuthProviderField): boolean {
    return !isEmptyVal(f.type, channelValues[id]?.[f.key]);
  }
  function overrideChip(c: Channel): string {
    if (!c.provider) {
      return "inherits shared";
    }
    const hit = simpleShareable(c.provider).find((f) => isOverridden(c.accountId, f));
    return hit ? hit.label.toLowerCase() + " overridden" : "inherits shared";
  }

  // Resolve a profile's provider: prefer the linked account's providerId, else match
  // the display platform against a provider id/displayName (mirrors StreamsTab).
  function resolveProvider(p: StreamProfileInfo, status: OAuthStatus | undefined): OAuthProvider | null {
    if (status?.connected) {
      const byId = providers.find((pv) => pv.id === status.providerId);
      if (byId) {
        return byId;
      }
    }
    const plat = (p.platform || "").trim().toLowerCase();
    if (!plat) {
      return null;
    }
    return providers.find((pv) => pv.id.toLowerCase() === plat || pv.displayName.toLowerCase() === plat) ?? null;
  }

  // Channels: enabled bindings -> their profile, grouped by accountId (the channel
  // identity). Profiles with no accountId (key/RTMP/WHIP) carry no channel metadata
  // and are dropped — they stream via key but are out of scope for this dialog. A
  // profile enabled on several canvases is deduped to one stream per profileUuid.
  const channels = $derived.by<Channel[]>(() => {
    const map = new Map<string, Channel>();
    for (const b of bindings) {
      if (!b.enabled || !b.profileUuid) {
        continue;
      }
      const profile = profiles.find((p) => p.uuid === b.profileUuid);
      if (!profile || !profile.accountId) {
        continue;
      }
      const accountId = profile.accountId;
      let ch = map.get(accountId);
      if (!ch) {
        // Status rows are keyed by accountId; one row per account.
        const status = statuses.find((s) => s.accountId === accountId);
        const provider = resolveProvider(profile, status);
        ch = {
          accountId,
          provider,
          status,
          login: status?.login || status?.displayName || profile.label,
          connected: !!(provider && status?.connected),
          needsReconnect: !!(provider && status?.needsReconnect && !status?.connected),
          streams: [],
        };
        map.set(accountId, ch);
      }
      if (!ch.streams.some((s) => s.profileUuid === b.profileUuid)) {
        ch.streams.push({ profileUuid: b.profileUuid, profile, label: profile.label, canvasName: b.canvasName });
      }
    }
    return [...map.values()];
  });
  // Only fully-connected channels are editable (prefill get + confirm set). A
  // needsReconnect channel is connected:false, so it is excluded here and still goes
  // live via its streams' keys.
  const connectedChannels = $derived(channels.filter((c) => c.connected));

  // Footer gating mirrors the old `armed.length === 0`: the modal (esp. Go Live) must
  // still work when only key-only profiles are enabled, so gate on the distinct count
  // of enabled profiles regardless of channel identity, not on connectedChannels.
  const armedProfileCount = $derived.by<number>(() => {
    const seen = new Set<string>();
    for (const b of bindings) {
      if (!b.enabled || !b.profileUuid || seen.has(b.profileUuid)) {
        continue;
      }
      if (profiles.find((p) => p.uuid === b.profileUuid)) {
        seen.add(b.profileUuid);
      }
    }
    return seen.size;
  });

  // Shared-defaults descriptor: the UNION of every shareable field across connected
  // providers, deduped by key (first provider's label/type wins). Drives the shared
  // block so a provider marking a new field shareable gets a shared source with no
  // edits here.
  const sharedFields = $derived.by<OAuthProviderField[]>(() => {
    const seen = new Set<string>();
    const out: OAuthProviderField[] = [];
    for (const c of connectedChannels) {
      if (!c.provider) {
        continue;
      }
      for (const f of c.provider.fields) {
        if (f.shareable && !seen.has(f.key)) {
          seen.add(f.key);
          out.push(f);
        }
      }
    }
    return out;
  });

  const primaryLabel = $derived(
    goLiveModal.mode === "golive" ? "Go Live now" : isLive ? "Update info" : "Save info",
  );
  const footerNote = $derived(
    goLiveModal.mode === "golive"
      ? "Metadata pushed to each platform, then the stream starts. A failure on one platform won't block going live."
      : "Metadata is pushed to each connected platform. A failure on one won't affect the others.",
  );

  // Resolve effective values through the three layers and push them: a stream
  // override wins over the channel default, which wins over the shared value
  // (shared only supplies shareable keys). Empty fields are never emitted at any
  // layer, so a provider that treats "present" as "set" can't blank a channel by
  // inheriting nothing. `stream` omitted (or its map empty) => channel default.
  function effectiveFields(c: Channel, stream: Stream | undefined): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    if (!c.provider) {
      return out;
    }
    const cv = channelValues[c.accountId] ?? {};
    const sv = stream ? (streamOverrides[stream.profileUuid] ?? {}) : {};
    for (const f of c.provider.fields) {
      if (!isEmptyVal(f.type, sv[f.key])) {
        out[f.key] = sv[f.key];
      } else if (!isEmptyVal(f.type, cv[f.key])) {
        out[f.key] = cv[f.key];
      } else if (f.shareable && !isEmptyVal(f.type, sharedValues[f.key])) {
        out[f.key] = sharedValues[f.key];
      }
    }
    return out;
  }

  // Best-effort prefill (fired, not awaited, so a slow get never blocks the open).
  // Per channel it pulls BOTH the remembered store (streamMeta.getSaved) and the live
  // provider metadata (streamMeta.get) and merges them: saved defaults are the base,
  // live values layer over them (live wins where present — Twitch/Kick report the
  // current title/category; saved fills the gaps and is the only source for YouTube,
  // whose live metadata is empty). Every seed is guarded so a value the user edits
  // before this resolves is never clobbered.
  async function prefill(): Promise<void> {
    await Promise.all(
      connectedChannels.map(async (c) => {
        const profileUuids = c.streams.map((s) => s.profileUuid);
        const [savedR, liveR] = await Promise.allSettled([
          obs.call("streamMeta.getSaved", { accountId: c.accountId, profileUuids }),
          obs.call("streamMeta.get", { accountId: c.accountId }),
        ]);
        const saved = savedR.status === "fulfilled" ? savedR.value : { channel: {}, streams: {} };
        const live = liveR.status === "fulfilled" ? liveR.value : undefined;

        // Channel bag: saved base, live over. Seed a key only when the current value
        // is empty (user hasn't touched it), matching the old title guard.
        const merged: Record<string, unknown> = { ...saved.channel };
        if (live?.title) {
          merged.title = live.title;
        }
        if (live?.category?.id) {
          merged.category = { id: live.category.id, name: live.category.name };
        }
        if (live?.language) {
          merged.language = live.language;
        }
        for (const [key, val] of Object.entries(merged)) {
          const type = c.provider?.fields.find((f) => f.key === key)?.type ?? "text";
          if (!isEmptyVal(type, val) && isEmptyVal(type, getVal(c.accountId, key))) {
            setField(c.accountId, key, val);
          }
        }

        // Restore remembered per-stream overrides and expand them in Advanced. Skip a
        // stream the user has already toggled or edited.
        for (const [uuid, bag] of Object.entries(saved.streams)) {
          if (bag && Object.keys(bag).length && !streamOverrideOn[uuid] && !streamOverrides[uuid]) {
            streamOverrides[uuid] = { ...bag };
            streamOverrideOn[uuid] = true;
          }
        }

        // Shared title: first channel to report one wins (prefer live, fall back to
        // saved). Guarded so a user-typed shared title is never overwritten.
        const savedTitle = typeof saved.channel.title === "string" ? saved.channel.title : "";
        const title = live?.title || savedTitle;
        if (title && !sharedValues.title) {
          sharedValues.title = title;
        }
      }),
    );
  }

  $effect(() => {
    let active = true;
    const offOauth = oauthStore.subscribe();
    // Gate prefill on all four data sources being ready so connectedChannels is populated
    // before the best-effort get runs (whenReady starts each store).
    Promise.all([
      oauthStore.whenReady(),
      outputBindingStore.whenReady(),
      streamProfileStore.whenReady(),
      obs.call("getStreamingState").catch(() => ({ active: false })),
    ]).then(([, , , st]) => {
      if (!active) {
        return;
      }
      isLive = !!st.active;
      loaded = true;
      void prefill();
    });
    const off = obs.on("streaming.changed", (p) => (isLive = p.active));
    return () => {
      active = false;
      offOauth();
      off();
    };
  });

  async function confirm(): Promise<void> {
    if (submitting) {
      return;
    }
    submitting = true;
    // One job per stream: each stream's effective fields merge the channel default
    // with that stream's own overrides. YouTube needs the per-profile call; Twitch/
    // Kick applying the same channel twice (no divergence) is idempotent.
    const jobs = connectedChannels.flatMap((c) =>
      c.streams.map((s) => ({ channel: c, stream: s, fields: effectiveFields(c, s) })),
    );
    const results = await Promise.allSettled(
      jobs.map((j) =>
        obs.call("streamMeta.set", {
          accountId: j.channel.accountId,
          profileUuid: j.stream.profileUuid,
          fields: j.fields,
        }),
      ),
    );
    // Partial-failure tolerance: a failed metadata push never blocks going live. One
    // aggregate toast (showToast replaces, so per-destination toasts would clobber
    // each other) names the channel(s) in human terms, not raw API strings.
    const failed = results
      .map((r, i) => (r.status === "rejected" ? { job: jobs[i], reason: r.reason as Error } : null))
      .filter((x): x is { job: (typeof jobs)[number]; reason: Error } => x !== null);
    const goingLive = goLiveModal.mode === "golive";
    const tail = goingLive ? " — going live anyway" : "";
    if (failed.length === 1) {
      const name = failed[0].job.channel.provider?.displayName || failed[0].job.stream.label || "this platform";
      showToast("Couldn't update " + name + " stream info" + tail, failed[0].reason?.message ?? "metadata push failed");
    } else if (failed.length > 1) {
      // Count distinct channels, not per-stream jobs: two failing streams on one
      // channel are one destination, so the count agrees with the listed names.
      const nameSet = new Set(failed.map((f) => f.job.channel.provider?.displayName || f.job.stream.label));
      const names = [...nameSet].join(", ");
      showToast("Couldn't update stream info for " + nameSet.size + " destinations" + tail, names);
    }
    // Remember these details for next time — best-effort, fired without awaiting so a
    // slow or failing save never blocks going live. One save per channel with its raw
    // layers: the channel bag plus only the streams carrying an enabled, non-empty
    // override.
    if (remember) {
      void Promise.allSettled(
        connectedChannels.map((c) => {
          const streams: Record<string, Record<string, unknown>> = {};
          for (const s of c.streams) {
            const ov = streamOverrides[s.profileUuid] ?? {};
            if (streamOverrideOn[s.profileUuid] && Object.keys(ov).length) {
              streams[s.profileUuid] = ov;
            }
          }
          return obs.call("streamMeta.save", {
            accountId: c.accountId,
            channel: channelValues[c.accountId] ?? {},
            streams,
          });
        }),
      ).then((rs) => {
        if (rs.some((r) => r.status === "rejected")) {
          console.warn("streamMeta.save: some channels failed to persist");
        }
      });
    }

    if (goLiveModal.mode === "golive") {
      try {
        await obs.call("streaming.start");
      } catch (e) {
        showToast("Go Live failed", (e as Error).message);
        submitting = false;
        return;
      }
    }
    submitting = false;
    closeGoLiveModal();
  }
</script>

<Modal title="Stream Information" onClose={closeGoLiveModal} width={580} maxHeight="88vh">
  {#snippet headExtra()}
    {#if isLive}<span class="live-dot" title="Live"></span>{/if}
    <Segmented options={VIEW_OPTIONS} value={view} onChange={(v) => (view = v as "simple" | "advanced")} />
  {/snippet}

  {#if !loaded}
        <p class="note">Loading destinations…</p>
      {:else}
        <!-- Shared defaults: the union of connected providers' shareable fields
             (mock `.shared`), present in both modes whenever anything is shareable. -->
        {#if sharedFields.length}
          <div class="shared">
            <p class="eh tight">Shared defaults</p>
            {#each sharedFields as f, i (f.key)}
              <div class="fld" class:last={i === sharedFields.length - 1}>
                <span class="fl">{f.label}</span>
                <GoLiveFieldInput field={f} value={sharedValues[f.key]} onChange={(v) => (sharedValues[f.key] = v)} />
              </div>
            {/each}
          </div>
        {/if}

        {#if armedProfileCount === 0}
          <p class="note">
            No armed destinations. Enable a destination on a canvas to push stream information.
          </p>
        {:else if view === "advanced"}
          <p class="eh">Per-channel (overrides shared when filled)</p>

          {#each connectedChannels as c (c.accountId)}
            {#if c.provider}
              <div class="dest">
                <div class="dh">
                  <span class="pdot" style:background={c.provider.brandColor || "var(--color-accent)"}></span>
                  <span class="pname">{c.provider.displayName}</span>
                  <span class="pacct">· {c.login}</span>
                  {#if c.streams.length > 1}
                    <span class="streams">{c.streams.length} streams</span>
                  {/if}
                  {#if overrideChip(c) === "inherits shared"}
                    <span class="inh">inherits shared</span>
                  {:else}
                    <span class="ovrflag">{overrideChip(c)}</span>
                  {/if}
                </div>
                <div class="body">
                  <!-- Shareable simple fields render as overrides (ghost / amber). -->
                  {#each simpleShareable(c.provider) as f (f.key)}
                    {@const filled = isOverridden(c.accountId, f)}
                    <div class="fld">
                      <span class="fl">{f.label}</span>
                      <GoLiveFieldInput
                        field={f}
                        value={getVal(c.accountId, f.key)}
                        providerId={c.provider.id}
                        ghostText={sharedGhostText(f)}
                        accent={filled}
                        onChange={(v) => setField(c.accountId, f.key, v)}
                      />
                      {#if filled}
                        <div class="hint acc">overrides shared for {c.provider.displayName}</div>
                      {:else}
                        <div class="hint">empty → using shared {f.label.toLowerCase()}</div>
                      {/if}
                    </div>
                  {/each}

                  <!-- Non-shareable simple fields render normally. -->
                  {#if simpleNonShareable(c.provider).length}
                    <div class="drow">
                      {#each simpleNonShareable(c.provider) as f (f.key)}
                        <div class="fld last">
                          <span class="fl">{f.label}</span>
                          <GoLiveFieldInput
                            field={f}
                            value={getVal(c.accountId, f.key)}
                            providerId={c.provider.id}
                            onChange={(v) => setField(c.accountId, f.key, v)}
                          />
                        </div>
                      {/each}
                    </div>
                  {/if}

                  <!-- Advanced / platform-only fields under the dashed divider. -->
                  <div class="adv">
                    <div class="advlbl">{c.provider.displayName}-only</div>
                    {#if advancedFields(c.provider).length === 0}
                      <p class="note">
                        {c.provider.displayName}'s API exposes only title / category / tags — nothing extra to show.
                      </p>
                    {:else}
                      {#each advancedFields(c.provider) as f (f.key)}
                        {#if f.type === "bool"}
                          <div class="togrow">
                            <GoLiveFieldInput
                              field={f}
                              value={getVal(c.accountId, f.key)}
                              onChange={(v) => setField(c.accountId, f.key, v)}
                            />
                            {f.label}
                          </div>
                        {:else}
                          <div class="fld last advfld">
                            <span class="fl">{f.label}</span>
                            <GoLiveFieldInput
                              field={f}
                              value={getVal(c.accountId, f.key)}
                              providerId={c.provider.id}
                              narrow={f.type === "enum"}
                              onChange={(v) => setField(c.accountId, f.key, v)}
                            />
                          </div>
                        {/if}
                      {/each}
                    {/if}
                  </div>
                </div>

                <!-- Per-stream overrides: each broadcast can diverge from the channel
                     default. OFF = inherit (no fields shown); ON = an inline field set
                     writing streamOverrides[profileUuid]. Shown even for a single
                     stream, kept unobtrusive. -->
                <div class="streamlist">
                  {#each c.streams as s (s.profileUuid)}
                    {@const on = !!streamOverrideOn[s.profileUuid]}
                    <div class="srow">
                      <div class="srh">
                        <span class="sico" class:on>{on ? "▾" : "▸"}</span>
                        <span class="sn">{s.label}</span>
                        <span class="scanvas">{s.canvasName}</span>
                        <span class="sbadge">
                          {#if on}
                            {@const n = streamOverrideCount(s.profileUuid, c.provider)}
                            <span class="sov">{n} override{n === 1 ? "" : "s"}</span>
                          {:else}
                            <span class="sinh">Uses channel defaults</span>
                          {/if}
                          <button
                            type="button"
                            class="sw"
                            class:on
                            title={on ? "Overriding channel defaults" : "Override channel defaults"}
                            onclick={() => toggleStreamOverride(s.profileUuid)}
                          >
                            <i></i>
                          </button>
                        </span>
                      </div>
                      {#if on}
                        <div class="srb">
                          {#each c.provider.fields as f (f.key)}
                            {#if f.type === "bool"}
                              <div class="togrow">
                                <GoLiveFieldInput
                                  field={f}
                                  value={getStreamVal(s.profileUuid, f.key)}
                                  onChange={(v) => setStreamField(s.profileUuid, f.key, v)}
                                />
                                {f.label}
                              </div>
                            {:else}
                              <div class="fld">
                                <span class="fl">{f.label}</span>
                                <GoLiveFieldInput
                                  field={f}
                                  value={getStreamVal(s.profileUuid, f.key)}
                                  providerId={c.provider.id}
                                  narrow={f.type === "enum"}
                                  onChange={(v) => setStreamField(s.profileUuid, f.key, v)}
                                />
                              </div>
                            {/if}
                          {/each}
                          <p class="inhnote">Empty fields inherit this channel's defaults.</p>
                        </div>
                      {/if}
                    </div>
                  {/each}
                </div>
              </div>
            {/if}
          {/each}
        {:else}
          <!-- Simple mode: shared block + only non-shareable per-channel fields. -->
          {#each connectedChannels as c (c.accountId)}
            {#if c.provider}
              <div class="simple-dest">
                <div class="sd-head">
                  <span class="pdot" style:background={c.provider.brandColor || "var(--color-accent)"}></span>
                  <span class="pname">{c.provider.displayName}</span>
                  <span class="pacct">· {c.login}</span>
                  {#if c.streams.length > 1}
                    <span class="streams">{c.streams.length} streams</span>
                  {/if}
                </div>
                {#if simpleNonShareable(c.provider).length}
                  <div class="drow">
                    {#each simpleNonShareable(c.provider) as f (f.key)}
                      <div class="fld last">
                        <span class="fl">{f.label}</span>
                        <GoLiveFieldInput
                          field={f}
                          value={getVal(c.accountId, f.key)}
                          providerId={c.provider.id}
                          onChange={(v) => setField(c.accountId, f.key, v)}
                        />
                      </div>
                    {/each}
                  </div>
                {:else}
                  <p class="note">Uses the shared defaults.</p>
                {/if}
              </div>
            {/if}
          {/each}
        {/if}
      {/if}

      {#if loaded}
        <div class="savebar">
          <button
            type="button"
            class="sw"
            class:on={remember}
            role="switch"
            aria-checked={remember}
            title={remember ? "Details will be remembered" : "Details won't be remembered"}
            onclick={() => (remember = !remember)}
          >
            <i></i>
          </button>
          <span class="sbl">Save these details for next time</span>
          <span class="sbh">prefill this dialog on your next go-live</span>
        </div>
      {/if}

  {#snippet footer()}
    <span class="foot-note">{footerNote}</span>
    <button class="accent" disabled={submitting || !loaded || armedProfileCount === 0} onclick={() => void confirm()}>
      {submitting ? "Working…" : primaryLabel}
    </button>
  {/snippet}
</Modal>

<style>
  .live-dot {
    flex: 0 0 auto;
    width: 7px;
    height: 7px;
    background: var(--color-live);
  }
  .eh {
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--color-dim);
    margin: 0 0 10px;
  }
  .eh.tight {
    margin-bottom: 8px;
  }
  .fl {
    display: block;
    font-size: 12px;
    color: var(--color-dim);
    margin-bottom: 4px;
  }
  .fld {
    margin-bottom: 10px;
  }
  .fld.last {
    margin-bottom: 0;
  }
  .drow {
    display: flex;
    gap: 10px;
    align-items: flex-start;
    flex-wrap: wrap;
  }
  .drow > * {
    flex: 1;
    min-width: 160px;
  }
  .shared {
    background: var(--color-surface-2);
    border: var(--border-weight) solid var(--color-border);
    padding: 12px;
    margin-bottom: 14px;
  }
  .dest {
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
    margin-bottom: 10px;
  }
  .dh {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 10px 12px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
  }
  .pdot {
    width: 8px;
    height: 8px;
    flex: 0 0 auto;
  }
  .pname {
    font-weight: 600;
  }
  .pacct {
    color: var(--color-dim);
    font-size: 12px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .ovrflag {
    margin-left: auto;
    font-size: 10px;
    color: var(--color-accent);
    text-transform: uppercase;
    letter-spacing: 0.05em;
    flex: 0 0 auto;
  }
  .inh {
    margin-left: auto;
    font-size: 10px;
    color: var(--color-muted);
    text-transform: uppercase;
    letter-spacing: 0.05em;
    flex: 0 0 auto;
  }
  .streams {
    font-size: 10px;
    color: var(--color-accent);
    border: var(--border-weight) solid var(--color-border);
    padding: 2px 6px;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    flex: 0 0 auto;
  }
  .body {
    padding: 12px;
  }
  .hint {
    font-size: 10px;
    color: var(--color-muted);
    margin-top: 3px;
  }
  .hint.acc {
    color: var(--color-accent);
  }
  .adv {
    margin-top: 10px;
    padding-top: 10px;
    border-top: var(--border-weight) dashed var(--color-border);
  }
  .advlbl {
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--color-accent);
    margin-bottom: 8px;
  }
  .advfld {
    margin-top: 6px;
  }
  .togrow {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 12px;
    margin-bottom: 6px;
  }
  .streamlist {
    border-top: var(--border-weight) solid var(--color-border);
  }
  .srow {
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .srow:last-child {
    border-bottom: 0;
  }
  .srh {
    display: flex;
    align-items: center;
    gap: 9px;
    padding: 9px 12px;
  }
  .sico {
    font-size: 9px;
    color: var(--color-muted);
    flex: 0 0 auto;
  }
  .sico.on {
    color: var(--color-accent);
  }
  .sn {
    font-size: 12px;
    font-weight: 600;
  }
  .scanvas {
    font-size: 11px;
    color: var(--color-dim);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .sbadge {
    margin-left: auto;
    display: flex;
    align-items: center;
    gap: 8px;
    flex: 0 0 auto;
  }
  .sinh {
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--color-muted);
  }
  .sov {
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--color-accent);
  }
  .sw {
    width: 32px;
    height: 17px;
    padding: 0;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
    position: relative;
    flex: 0 0 auto;
    cursor: pointer;
  }
  .sw.on {
    border-color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 28%, transparent);
  }
  .sw i {
    position: absolute;
    top: 1px;
    left: 1px;
    width: 13px;
    height: 13px;
    background: var(--color-muted);
  }
  .sw.on i {
    left: auto;
    right: 1px;
    background: var(--color-accent);
  }
  .srb {
    padding: 8px 12px 12px 33px;
    background: var(--color-surface-2);
  }
  .inhnote {
    font-size: 10px;
    color: var(--color-muted);
    margin: 4px 0 0;
  }
  .simple-dest {
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
    padding: 12px;
    margin-bottom: 10px;
  }
  .sd-head {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 10px;
  }
  .note {
    font-size: 11px;
    color: var(--color-muted);
    margin: 0;
  }
  .foot-note {
    flex: 1 1 auto;
    font-size: 11px;
    color: var(--color-muted);
  }
  /* Persisted just above the footer: sticks to the bottom of the scroll body and
     bleeds to its edges (cancelling the body padding) so it reads as a distinct
     strip between the scrollable content and the action bar. */
  .savebar {
    position: sticky;
    bottom: -16px;
    display: flex;
    align-items: center;
    gap: 10px;
    margin: 14px -14px -16px;
    padding: 10px 14px;
    border-top: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
  }
  .sbl {
    font-size: 11px;
    color: var(--color-dim);
  }
  .sbh {
    font-size: 10px;
    color: var(--color-muted);
  }
</style>
