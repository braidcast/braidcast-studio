<script lang="ts">
  import {
    obs,
    type OAuthProvider,
    type OAuthProviderField,
    type OAuthStatus,
    type OutputBindingInfo,
    type StreamProfileInfo,
  } from "./bridge";
import { EV } from "./eventNames";
  import { goLiveModal, closeGoLiveModal } from "./goLiveModalOpener.svelte";
  import { openOAuthConnect } from "./oauthConnectOpener.svelte";
  import { outputBindingStore } from "./outputBindingStore.svelte";
  import { streamProfileStore } from "./streamProfileStore.svelte";
  import { oauthStore } from "./oauthStore.svelte";
  import { showToast } from "./toastStore.svelte";
  import Avatar from "./Avatar.svelte";
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

  // Per-channel save outcome, keyed by accountId, surfaced as the header chip. Set to
  // "saving" while confirm() awaits each channel's streamMeta.set, then mapped from the
  // allSettled results back to each channel ("saved" unless any of its streams failed).
  type SaveState = "idle" | "saving" | "saved" | "error";
  let channelSaveState = $state<Record<string, SaveState>>({});
  const SAVE_LABEL: Record<SaveState, string> = {
    idle: "",
    saving: "Saving…",
    saved: "Saved",
    error: "Failed",
  };
  // Channel cards collapse to just their header. Default expanded; the caret toggles.
  let collapsed = $state<Record<string, boolean>>({});
  function toggleCollapsed(id: string): void {
    collapsed[id] = !collapsed[id];
  }

  // Reconnect a stale-scope channel via the shared OAuth connect dialog (the same flow
  // the Streams tab uses). Any of the channel's stream profiles carries the account, so
  // the first drives the relink; on success the store refreshes and the card re-renders
  // as connected. The needsReconnect channel is skipped by prefill/confirm/save until then.
  function reconnect(c: Channel): void {
    const first = c.streams[0];
    if (!first || !c.provider) {
      return;
    }
    openOAuthConnect({ profileUuid: first.profileUuid, providerId: c.provider.id, platformName: c.provider.displayName });
  }

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

  // Shared-block keys the user has edited by hand. Prefill must never seed or diverge
  // a key the user owns, otherwise a shared edit made while the (fired-not-awaited)
  // get/getSaved are in flight would be silently overridden by a stale live value.
  const touchedShared = new Set<string>();
  function setSharedField(key: string, val: unknown): void {
    touchedShared.add(key);
    sharedValues = { ...sharedValues, [key]: val };
  }
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

  // Type-aware value equality, used to tell a genuine per-channel divergence from a
  // value that merely echoes the shared default. Plain === is wrong for category (two
  // equal {id,name} objects are distinct references) and tags (array identity), which
  // would reintroduce the spurious "overrides shared" chip.
  function valuesEqual(type: string, a: unknown, b: unknown): boolean {
    if (type === "category") {
      const ai = a && typeof a === "object" ? (a as { id?: string }).id : undefined;
      const bi = b && typeof b === "object" ? (b as { id?: string }).id : undefined;
      return ai === bi;
    }
    if (type === "tags" || type === "labelset") {
      const aa = Array.isArray(a) ? [...(a as unknown[])].sort() : [];
      const bb = Array.isArray(b) ? [...(b as unknown[])].sort() : [];
      return aa.length === bb.length && aa.every((v, i) => v === bb[i]);
    }
    return a === b;
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

  // Channels that go live via key but can't push metadata until relinked, plus the total
  // stream count across every channel — both drive the footer summary and reconnect strips.
  const reconnectChannels = $derived(channels.filter((c) => c.needsReconnect));
  const channelStreamCount = $derived(channels.reduce((n, c) => n + c.streams.length, 0));
  // Footer summary (mock: "3 channels · 5 streams · all ready"). When any channel needs a
  // relink, count ready vs. need-reconnect instead so the strips are accounted for. Falls
  // back to a key-only line when no account-linked channels are armed.
  const footerNote = $derived.by<string>(() => {
    if (channels.length === 0) {
      return armedProfileCount > 0
        ? `${armedProfileCount} destination${armedProfileCount === 1 ? "" : "s"} via stream key`
        : "No destinations armed.";
    }
    const streams = `${channelStreamCount} stream${channelStreamCount === 1 ? "" : "s"}`;
    if (reconnectChannels.length > 0) {
      const ready = `${connectedChannels.length} ready`;
      return `${ready} · ${streams} · ${reconnectChannels.length} need reconnect`;
    }
    const chans = `${channels.length} channel${channels.length === 1 ? "" : "s"}`;
    return `${chans} · ${streams} · all ready`;
  });

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
        // Default channel/streams to {} even on the fulfilled path so a malformed
        // response missing a field can't throw and reject the whole prefill.
        const savedRaw = savedR.status === "fulfilled" ? savedR.value : undefined;
        const saved = { channel: savedRaw?.channel ?? {}, streams: savedRaw?.streams ?? {} };
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
        // Route each merged key by tier so shareable values land in the SHARED layer
        // (first channel wins), not as a spurious per-channel override: a channel only
        // takes an override when it genuinely diverges from the shared value. Without
        // this, two channels live-reporting the same title would each read as
        // "overrides shared". Non-shareable keys stand alone in channelValues. Every
        // write is guarded so a user edit before this resolves is never clobbered.
        for (const [key, val] of Object.entries(merged)) {
          const f = c.provider?.fields.find((fd) => fd.key === key);
          const type = f?.type ?? "text";
          if (isEmptyVal(type, val)) {
            continue;
          }
          if (f?.shareable) {
            if (touchedShared.has(key)) {
              continue;
            }
            if (isEmptyVal(type, sharedValues[key])) {
              sharedValues = { ...sharedValues, [key]: val };
            } else if (!valuesEqual(type, sharedValues[key], val) && isEmptyVal(type, getVal(c.accountId, key))) {
              setField(c.accountId, key, val);
            }
          } else if (isEmptyVal(type, getVal(c.accountId, key))) {
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
    const off = obs.on(EV.streamingChanged, (p) => (isLive = p.active));
    // External metadata edit (MCP / another window / a prior apply): re-run the
    // best-effort prefill so still-empty fields pick up the fresh values. Prefill's
    // touched/non-empty guards keep it from clobbering anything the user has edited.
    const offMeta = obs.on(EV.streamMetaChanged, () => {
      if (loaded) void prefill();
    });
    return () => {
      active = false;
      offOauth();
      off();
      offMeta();
    };
  });

  async function confirm(): Promise<void> {
    if (submitting) {
      return;
    }
    submitting = true;
    // Header chips go "saving" for every connected channel while the pushes are in flight.
    channelSaveState = Object.fromEntries(
      connectedChannels.map((c) => [c.accountId, "saving"] as [string, SaveState]),
    );
    // One job per stream: each stream's effective fields merge the channel default
    // with that stream's own overrides. YouTube needs the per-profile call; Twitch/
    // Kick applying the same channel twice (no divergence) is idempotent. The stream
    // layer applies ONLY when its override toggle is on, so an orphaned overrides bag
    // (toggle off) can never diverge a live broadcast — the toggle is the sole authority.
    const jobs = connectedChannels.flatMap((c) =>
      c.streams.map((s) => ({
        channel: c,
        stream: s,
        fields: effectiveFields(c, streamOverrideOn[s.profileUuid] ? s : undefined),
      })),
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
    // Collapse failures to distinct CHANNELS: two failing streams on one channel are one
    // destination, so the count, the singular/plural branch, and the header chip all agree.
    const failedByChannel = new Map<string, { name: string; reason: Error }>();
    for (const f of failed) {
      if (!failedByChannel.has(f.job.channel.accountId)) {
        failedByChannel.set(f.job.channel.accountId, {
          name: f.job.channel.provider?.displayName || f.job.stream.label || "this platform",
          reason: f.reason,
        });
      }
    }
    channelSaveState = Object.fromEntries(
      connectedChannels.map(
        (c) => [c.accountId, failedByChannel.has(c.accountId) ? "error" : "saved"] as [string, SaveState],
      ),
    );
    const goingLive = goLiveModal.mode === "golive";
    // Stream info is a precondition, not a courtesy: if any channel's metadata push
    // failed, going live would stream with stale/wrong title+category. Block the start
    // on ANY failure (all OAuth providers) and keep the modal open so the user can retry
    // or fix the offending channel. Update-info mode has no start to block, so it only
    // reports the failure.
    if (failedByChannel.size > 0) {
      const lead = goingLive ? "Not going live — couldn't update " : "Couldn't update ";
      if (failedByChannel.size === 1) {
        const only = [...failedByChannel.values()][0];
        showToast(lead + only.name + " stream info", only.reason?.message ?? "metadata push failed");
      } else {
        const names = [...failedByChannel.values()].map((v) => v.name).join(", ");
        showToast(lead + failedByChannel.size + " destinations", names);
      }
      if (goingLive) {
        submitting = false;
        return;
      }
    }
    // Remember these details for next time — best-effort, fired without awaiting so a
    // slow or failing save never blocks going live. One save per channel with its raw
    // layers: the channel bag plus the channel's COMPLETE stream set. Streams with an
    // enabled, non-empty override carry their bag; every other stream carries {} so the
    // store clears any override toggled off this session (otherwise it would resurrect
    // and re-apply on the next go-live).
    if (remember) {
      void Promise.allSettled(
        connectedChannels.map((c) => {
          const streams: Record<string, Record<string, unknown>> = {};
          for (const s of c.streams) {
            const ov = streamOverrides[s.profileUuid] ?? {};
            streams[s.profileUuid] =
              streamOverrideOn[s.profileUuid] && Object.keys(ov).length ? ov : {};
          }
          return obs.call("streamMeta.save", {
            accountId: c.accountId,
            // Persist the EFFECTIVE channel-level defaults (shared shareable keys
            // merged with channel values, empties dropped) so shared-block values —
            // which live in sharedValues, not channelValues — are actually saved.
            // Prefill routes them back into the shared layer (first-wins + dedup).
            channel: effectiveFields(c, undefined),
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

<!-- One field row, shared by the shared block, the channel body, and the per-stream
     override set (was ~3 copies of the bool/togrow-vs-labelled-field branch). `opts`
     carries the per-site extras (provider id for the category typeahead, the inherit
     ghost + accent for override fields, an optional hint line, narrow for enums). -->
{#snippet fieldRow(
  f: OAuthProviderField,
  value: unknown,
  onChange: (v: unknown) => void,
  opts: {
    providerId?: string;
    ghostText?: string;
    accent?: boolean;
    narrow?: boolean;
    hint?: string;
    tag?: string;
  },
)}
  {#if f.type === "bool"}
    <div class="togrow">
      <GoLiveFieldInput field={f} {value} {onChange} />
      <span class="toglbl">{f.label}</span>
    </div>
  {:else}
    <div class="field">
      <span class="fl">
        {f.label}{#if opts.tag}<span class="fl-tag" class:acc={opts.accent}>{opts.tag}</span>{/if}
      </span>
      <GoLiveFieldInput
        field={f}
        {value}
        {onChange}
        providerId={opts.providerId ?? ""}
        ghostText={opts.ghostText ?? ""}
        accent={opts.accent ?? false}
        narrow={opts.narrow ?? false}
      />
      {#if opts.hint}<div class="hint" class:acc={opts.accent}>{opts.hint}</div>{/if}
    </div>
  {/if}
{/snippet}

<Modal
  title="Stream Information"
  onClose={closeGoLiveModal}
  width={view === "advanced" ? 760 : 580}
  maxHeight="88vh"
>
  {#snippet headExtra()}
    {#if isLive}<span class="live-dot" title="Live"></span>{/if}
    <Segmented options={VIEW_OPTIONS} value={view} onChange={(v) => (view = v as "simple" | "advanced")} />
  {/snippet}

  <div class="mb" class:adv={view === "advanced"}>
    {#if !loaded}
      <p class="note">Loading destinations…</p>
    {:else}
      <!-- Shared defaults: the union of connected providers' shareable fields (mock
           `.shared`), one row each, present in both modes whenever anything is shareable. -->
      {#if sharedFields.length}
        <div class="shared">
          <p class="eh">
            Shared defaults
            {#if connectedChannels.length > 1}<span class="eh-note">— across all {connectedChannels.length} channels</span>{/if}
          </p>
          {#each sharedFields as f (f.key)}
            {@render fieldRow(f, sharedValues[f.key], (v) => setSharedField(f.key, v), {})}
          {/each}
        </div>
      {/if}

      {#if armedProfileCount === 0}
        <p class="note">No armed destinations. Enable a destination on a canvas to push stream information.</p>
      {:else}
        {#each channels as c (c.accountId)}
          {#if c.connected && c.provider}
            {@const isCollapsed = !!collapsed[c.accountId]}
            {@const stt = channelSaveState[c.accountId]}
            <div class="ch">
              <button
                type="button"
                class="chh"
                class:nb={isCollapsed}
                aria-expanded={!isCollapsed}
                onclick={() => toggleCollapsed(c.accountId)}
              >
                <Avatar url={c.status?.avatarUrl ?? ""} name={c.provider.displayName || c.login} size={20} />
                <span class="plat">{c.provider.displayName}</span>
                <span class="nm">{c.login}</span>
                {#if c.streams.length > 1}
                  <span class="streams">{c.streams.length} streams</span>
                {/if}
                {#if stt && stt !== "idle"}
                  <span class="st" class:saving={stt === "saving"} class:ok={stt === "saved"} class:err={stt === "error"}>
                    <span class="dot"></span>{SAVE_LABEL[stt]}
                  </span>
                {/if}
                <span class="car">{isCollapsed ? "▸" : "▾"}</span>
              </button>

              {#if !isCollapsed}
                <div class="chb">
                  {#if c.streams.length > 1}
                    <div class="dedupe">
                      All <b>{c.streams.length} streams</b> post to this one channel — its title, category and thumbnail
                      are set once here.
                    </div>
                  {/if}

                  <!-- Shareable fields as per-channel overrides (ghost cue when inheriting
                       the shared value, amber when the channel diverges). -->
                  {#each simpleShareable(c.provider) as f (f.key)}
                    {@const filled = isOverridden(c.accountId, f)}
                    {@render fieldRow(f, getVal(c.accountId, f.key), (v) => setField(c.accountId, f.key, v), {
                      providerId: c.provider.id,
                      ghostText: sharedGhostText(f),
                      accent: filled,
                      tag: filled ? "— overrides shared" : "↳ using shared",
                      hint: filled ? "Overrides the shared " + f.label.toLowerCase() + " for this channel." : undefined,
                    })}
                  {/each}

                  <!-- Non-shareable fields (category / thumbnail / privacy / …), one per row. -->
                  {#each simpleNonShareable(c.provider) as f (f.key)}
                    {@render fieldRow(f, getVal(c.accountId, f.key), (v) => setField(c.accountId, f.key, v), {
                      providerId: c.provider.id,
                    })}
                  {/each}

                  {#if view === "advanced"}
                    <!-- Advanced / platform-only fields under the dashed divider. -->
                    {#if advancedFields(c.provider).length}
                      <div class="adv-fields">
                        <div class="advlbl">{c.provider.displayName}-only</div>
                        {#each advancedFields(c.provider) as f (f.key)}
                          {@render fieldRow(f, getVal(c.accountId, f.key), (v) => setField(c.accountId, f.key, v), {
                            providerId: c.provider.id,
                            narrow: f.type === "enum",
                          })}
                        {/each}
                      </div>
                    {/if}

                    <!-- Per-stream overrides: OFF = inherit the channel default; ON = an
                         inline field set writing streamOverrides[profileUuid]. -->
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
                                {@render fieldRow(f, getStreamVal(s.profileUuid, f.key), (v) => setStreamField(s.profileUuid, f.key, v), {
                                  providerId: c.provider.id,
                                  narrow: f.type === "enum",
                                })}
                              {/each}
                              <p class="inhnote">Empty fields inherit this channel's defaults.</p>
                            </div>
                          {/if}
                        </div>
                      {/each}
                    </div>
                  {/if}
                </div>
              {/if}
            </div>
          {:else if c.needsReconnect}
            <!-- Stale-scope channel: still goes live via key, but can't push metadata
                 until relinked. Kept visible as a warn strip with a Reconnect action
                 (the shared OAuth connect flow), never silently dropped. -->
            <div class="ch warn">
              <div class="warnstrip">
                <Avatar url={c.status?.avatarUrl ?? ""} name={c.provider?.displayName || c.login} size={20} />
                <span class="msg">
                  <b>{c.login}</b> — reconnect to edit
                  {c.provider?.displayName ?? "this platform"} stream info
                </span>
                <button type="button" class="rbtn" onclick={() => reconnect(c)}>Reconnect</button>
              </div>
            </div>
          {/if}
        {/each}
      {/if}
    {/if}

    {#if loaded}
      <div class="savebar">
        <button
          type="button"
          class="sw big"
          class:on={remember}
          role="switch"
          aria-checked={remember}
          title={remember ? "Details will be remembered" : "Details won't be remembered"}
          onclick={() => (remember = !remember)}
        >
          <i></i>
        </button>
        <span class="sbl">Save these details for next time</span>
        <span class="sbh">— prefill this dialog on your next go-live</span>
      </div>
    {/if}
  </div>

  {#snippet footer()}
    <span class="foot-note">{footerNote}</span>
    <button class="ghost" onclick={closeGoLiveModal}>Cancel</button>
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
  .note {
    font-size: 11px;
    color: var(--color-muted);
    margin: 0;
  }
  /* Body wrapper: no padding of its own (Modal's .modal-body owns the scroll + pad);
     the `adv` flag caps the wider Advanced modal's text inputs so they don't stretch
     edge-to-edge. */
  .mb {
    display: block;
  }
  .mb.adv .ch .field :global(input.inp),
  .mb.adv .ch .field :global(select.inp) {
    max-width: 460px;
  }

  /* Section head ("Shared defaults") — mono micro-label with an optional plain-text note. */
  .eh {
    display: flex;
    align-items: baseline;
    gap: 8px;
    flex-wrap: wrap;
    font-family: var(--font-mono);
    font-size: 10px;
    font-weight: 600;
    letter-spacing: 0.07em;
    text-transform: uppercase;
    color: var(--color-muted);
    margin: 0 0 8px;
  }
  .eh-note {
    font-family: var(--font-ui);
    font-size: 10px;
    letter-spacing: 0;
    text-transform: none;
    font-weight: 400;
    color: var(--color-muted);
  }
  .shared {
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
    padding: 12px;
    margin-bottom: 14px;
  }

  /* Field row (label + control + optional hint). Label is a mono micro-caps line; a
     plain-text tag (inherit / override cue) trails it. */
  .field {
    margin-bottom: 12px;
  }
  .field:last-child {
    margin-bottom: 0;
  }
  .fl {
    display: flex;
    align-items: baseline;
    gap: 7px;
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.07em;
    text-transform: uppercase;
    color: var(--color-muted);
    margin-bottom: 5px;
  }
  .fl-tag {
    font-family: var(--font-ui);
    font-size: 10px;
    letter-spacing: 0;
    text-transform: none;
    font-weight: 400;
    color: var(--color-muted);
  }
  .fl-tag.acc {
    color: var(--color-accent);
  }
  .hint {
    font-size: 10px;
    line-height: 1.4;
    color: var(--color-muted);
    margin-top: 4px;
  }
  .hint.acc {
    color: var(--color-accent);
  }
  .togrow {
    display: flex;
    align-items: center;
    gap: 9px;
    margin-bottom: 12px;
  }
  .togrow:last-child {
    margin-bottom: 0;
  }
  .toglbl {
    font-size: 11px;
    color: var(--color-dim);
  }

  /* Channel card. */
  .ch {
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
    margin-bottom: 12px;
  }
  .ch.warn {
    border-color: color-mix(in srgb, var(--color-warn) 45%, var(--color-border));
  }
  .chh {
    display: flex;
    align-items: center;
    gap: 9px;
    width: 100%;
    height: auto;
    padding: 9px 11px;
    background: var(--color-surface-2);
    border: 0;
    border-bottom: var(--border-weight) solid var(--color-border);
    text-align: left;
    cursor: pointer;
  }
  .chh:hover {
    border-color: var(--color-border);
  }
  .chh.nb {
    border-bottom: 0;
  }
  .plat {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    color: var(--color-muted);
    flex: 0 0 auto;
  }
  .nm {
    font-weight: 700;
    font-size: 12px;
    color: var(--color-text);
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .streams {
    font-family: var(--font-mono);
    font-size: 9.5px;
    color: var(--color-accent);
    border: var(--border-weight) solid color-mix(in srgb, var(--color-accent) 40%, var(--color-border));
    padding: 3px 6px;
    flex: 0 0 auto;
  }
  .st {
    display: flex;
    align-items: center;
    gap: 5px;
    margin-left: 4px;
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.04em;
    text-transform: uppercase;
    color: var(--color-muted);
    flex: 0 0 auto;
  }
  .st.saving {
    color: var(--color-dim);
  }
  .st.ok {
    color: var(--color-ok);
  }
  .st.err {
    color: var(--color-warn);
  }
  .st .dot {
    width: 6px;
    height: 6px;
    background: currentColor;
    flex: 0 0 auto;
  }
  .car {
    margin-left: auto;
    color: var(--color-muted);
    font-size: 11px;
    flex: 0 0 auto;
  }
  .chb {
    padding: 11px;
  }
  .dedupe {
    font-size: 10px;
    line-height: 1.4;
    color: var(--color-muted);
    margin: 0 0 11px;
    padding: 6px 9px;
    background: var(--color-base);
    border: var(--border-weight) dashed var(--color-border);
  }
  .dedupe b {
    color: var(--color-dim);
    font-weight: 600;
  }

  /* Advanced: platform-only fields under a dashed divider. */
  .adv-fields {
    margin-top: 12px;
    padding-top: 11px;
    border-top: var(--border-weight) dashed var(--color-border);
  }
  .advlbl {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    color: var(--color-accent);
    margin-bottom: 9px;
  }

  /* Per-stream override list (bleeds to the card edges below the channel fields). */
  .streamlist {
    margin: 11px -11px -11px;
    border-top: var(--border-weight) solid var(--color-border);
  }
  .srow {
    border-bottom: var(--border-weight) solid var(--color-border-2);
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
    font-size: 11px;
    font-weight: 600;
    color: var(--color-text);
    flex: 0 0 auto;
  }
  .scanvas {
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--color-muted);
    min-width: 0;
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
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.04em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .sov {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.04em;
    text-transform: uppercase;
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
  .sw:hover {
    border-color: var(--color-accent);
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
  .sw.big {
    width: 34px;
    height: 18px;
  }
  .sw.big i {
    width: 14px;
    height: 14px;
  }
  .srb {
    padding: 2px 12px 12px 33px;
    background: var(--color-base);
  }
  .inhnote {
    font-size: 10px;
    line-height: 1.4;
    color: var(--color-muted);
    margin: 8px 0 0;
  }

  /* Reconnect strip (stale-scope channel). */
  .warnstrip {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 11px;
  }
  .warnstrip .msg {
    font-size: 11px;
    line-height: 1.4;
    color: var(--color-warn);
  }
  .warnstrip .msg b {
    color: var(--color-text);
    font-weight: 600;
  }
  .rbtn {
    height: 28px;
    padding: 0 11px;
    margin-left: auto;
    border: var(--border-weight) solid var(--color-warn);
    background: var(--color-surface);
    color: var(--color-warn);
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.05em;
    text-transform: uppercase;
    flex: 0 0 auto;
  }
  .rbtn:hover {
    background: color-mix(in srgb, var(--color-warn) 14%, transparent);
    border-color: var(--color-warn);
  }

  /* Save-for-next-time strip: sticks to the bottom of the scroll body and bleeds to
     its edges (cancelling the body padding) so it reads as a distinct strip between
     the scrollable content and the action bar. */
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

  /* Footer bar. */
  .foot-note {
    flex: 1 1 auto;
    font-size: 11px;
    color: var(--color-muted);
  }
  .ghost {
    font-family: var(--font-mono);
    font-size: 11px;
    letter-spacing: 0.05em;
    text-transform: uppercase;
    color: var(--color-dim);
    background: var(--color-surface);
  }
  .ghost:hover {
    color: var(--color-text);
    border-color: var(--color-border);
  }
</style>
