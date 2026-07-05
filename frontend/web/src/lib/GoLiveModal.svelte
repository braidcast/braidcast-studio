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

  // Shared defaults every destination inherits (mock "Shared defaults" block),
  // keyed by field key. Driven by the union of connected providers' shareable
  // fields (see `sharedFields`), so any shareable field — not just title/tags —
  // gets a shared source.
  let shared = $state<Record<string, unknown>>({});

  // Per-channel overrides, keyed by accountId then field key. An empty shareable
  // field inherits the shared value; a non-shareable field stands alone. One field
  // set per channel is applied to every stream in that channel on confirm.
  let perDest = $state<Record<string, Record<string, unknown>>>({});

  function setField(id: string, key: string, val: unknown): void {
    perDest[id] = { ...(perDest[id] ?? {}), [key]: val };
  }
  function getVal(id: string, key: string): unknown {
    return perDest[id]?.[key];
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
    const v = shared[f.key];
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
    return !isEmptyVal(f.type, perDest[id]?.[f.key]);
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

  // Resolve effective values per the inheritance rule and push them. Shareable empty
  // -> shared value (omitted if the shared value is also empty); non-shareable -> its
  // own value (omitted when empty). Empty fields are never emitted, so a provider
  // that treats "present" as "set" can't blank a channel by inheriting nothing.
  function effectiveFields(c: Channel): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    if (!c.provider) {
      return out;
    }
    const pd = perDest[c.accountId] ?? {};
    for (const f of c.provider.fields) {
      const v = pd[f.key];
      if (f.shareable) {
        if (!isEmptyVal(f.type, v)) {
          out[f.key] = v;
        } else if (!isEmptyVal(f.type, shared[f.key])) {
          out[f.key] = shared[f.key];
        }
      } else if (!isEmptyVal(f.type, v)) {
        out[f.key] = v;
      }
    }
    return out;
  }

  // Best-effort prefill (fired, not awaited, so a slow get never blocks the open):
  // seed each connected channel's category (once per channel, not per stream) + seed
  // the shared title from the first platform that reports one. Per-channel titles stay
  // empty (inherit).
  async function prefill(): Promise<void> {
    await Promise.all(
      connectedChannels.map(async (c) => {
        try {
          const m = await obs.call("streamMeta.get", { accountId: c.accountId });
          if (m.category?.id) {
            setField(c.accountId, "category", { id: m.category.id, name: m.category.name });
          }
          if (m.title && !shared.title) {
            shared.title = m.title;
          }
        } catch {
          // Ignore: prefill is best-effort.
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
    // One job per stream: the channel's effective fields (computed once) are applied
    // to every stream in it. YouTube needs the per-profile call; Twitch/Kick applying
    // the same channel twice is idempotent.
    const jobs = connectedChannels.flatMap((c) => {
      const fields = effectiveFields(c);
      return c.streams.map((s) => ({ channel: c, stream: s, fields }));
    });
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
      const names = [
        ...new Set(failed.map((f) => f.job.channel.provider?.displayName || f.job.stream.label)),
      ].join(", ");
      showToast("Couldn't update stream info for " + failed.length + " destinations" + tail, names);
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
                <GoLiveFieldInput field={f} value={shared[f.key]} onChange={(v) => (shared[f.key] = v)} />
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
</style>
