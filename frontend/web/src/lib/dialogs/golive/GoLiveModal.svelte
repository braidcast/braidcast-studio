<script lang="ts">
  import {
    obs,
    type BridgeError,
    type OAuthProvider,
    type OAuthProviderField,
    type OAuthStatus,
    type StreamProfileInfo,
  } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { goLiveModal, closeGoLiveModal } from "$lib/dialogs/golive/goLiveModalOpener.svelte";
  import { openOAuthConnect, isOAuthConnecting } from "$lib/dialogs/oauthConnectOpener.svelte";
  import { canvasStore } from "$lib/stores/canvasStore.svelte";
  import {
    destinationIdentityStore,
    type DestinationIdentity,
  } from "$lib/stores/destinationIdentityStore.svelte";
  import { outputBindingStore } from "$lib/stores/outputBindingStore.svelte";
  import { streamProfileStore } from "$lib/stores/streamProfileStore.svelte";
  import { oauthStore, isStaleToken } from "$lib/stores/oauthStore.svelte";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import Avatar from "$lib/ui/Avatar.svelte";
  import GoLiveFieldInput from "$lib/dialogs/golive/GoLiveFieldInput.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import Modal from "$lib/ui/Modal.svelte";
  import PlatformMark from "$lib/ui/PlatformMark.svelte";
  import Segmented from "$lib/ui/Segmented.svelte";

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
  // same channel; `canvasName` is the canvas this stream is CONFIGURED on, armed or
  // not — null only when that canvas doesn't exist.
  interface Stream {
    profileUuid: string;
    profile: StreamProfileInfo;
    label: string;
    canvasName: string | null;
  }

  // One channel = one account identity (`accountId` = "providerId:userId"). Profiles
  // that reuse the same account collapse into this single card; `streams` lists them.
  // `connected` (provider resolved + OAuth linked) gates the editable card.
  interface Channel {
    accountId: string;
    provider: OAuthProvider | null;
    status: OAuthStatus | undefined;
    login: string;
    // Channel identity (avatar + the one displayName fallback chain) from the shared
    // destination join. Every profile on this card carries the same accountId, so one
    // lookup answers for the whole card. `login` stays the OAuth login the toasts and
    // the reconnect strip name the account by.
    identity: DestinationIdentity | null;
    connected: boolean;
    // Token scopes are stale: the backend refuses streamMeta, so this channel is
    // connected:false — excluded from the push. It still goes live via key.
    needsReconnect: boolean;
    // Any of this channel's bindings enabled. Independent of the save chip: a channel
    // can be failed AND armed; disarmed just means none of its bindings will start.
    armed: boolean;
    // Every binding uuid feeding this channel (a profile enabled on several canvases,
    // and/or several profiles on one account) — the arm toggle fans out over all of
    // them so on/off is never a partial state.
    bindingUuids: string[];
    streams: Stream[];
  }

  // One wording for a stream count, so the header badge and the footer summary can't
  // disagree about the same number.
  function streamsLabel(n: number): string {
    return `${n} stream${n === 1 ? "" : "s"}`;
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
  // Last push failure per accountId, rendered as a persistent strip in the channel
  // card. The toast can't carry it: showToast's second arg is hover-title only and
  // the visible line is a 4s nowrap one-liner, so reason + remedy must live here.
  let channelSaveError = $state<Record<string, string>>({});
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

  // Arm/disarm a channel = the same persistent outputBinding.setEnabled the
  // destinations panel does (via the shared store fan-out), never a session-only
  // skip. No optimistic local state: setEnabled awaits a store refresh, so the flip
  // flows back through `bindings` -> `channels`. Re-arming re-runs prefill because a
  // channel disarmed at open was skipped by it; prefill's guards keep the late seed
  // from clobbering anything already edited.
  let armBusy = $state<Record<string, boolean>>({});
  async function toggleArmed(c: Channel): Promise<void> {
    if (armBusy[c.accountId]) {
      return;
    }
    armBusy[c.accountId] = true;
    const arming = !c.armed;
    try {
      await outputBindingStore.setEnabled(c.bindingUuids, arming);
      // Either direction changes the go-live set, so the last attempt's outcome no
      // longer describes this channel — a stale chip/reason would sit beside a
      // switch the user just acted on.
      delete channelSaveState[c.accountId];
      delete channelSaveError[c.accountId];
      if (arming) {
        void prefill();
      }
    } catch (e) {
      showToast("Couldn't update " + (c.provider?.displayName ?? c.login), (e as Error).message);
    } finally {
      armBusy[c.accountId] = false;
    }
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

  // Disables the Reconnect button while this channel's connect flow is already open
  // (keyed off its first stream's profile, mirroring reconnect()'s own resolution),
  // so a double-click can't stack a second open.
  function reconnectBusy(c: Channel): boolean {
    const first = c.streams[0];
    return !!first && isOAuthConnecting(first.profileUuid);
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

  // Channels: ALL bindings (enabled or not) -> their profile, grouped by accountId
  // (the channel identity). Disabled bindings are kept — carried as `armed:false` —
  // so a switched-off destination still renders here and can be re-armed, or a
  // failing one disarmed, without leaving the modal. Profiles with no accountId
  // (key/RTMP/WHIP) carry no channel metadata and are dropped — they stream via key
  // but are out of scope for this dialog. A profile enabled on several canvases is
  // deduped to one stream per profileUuid.
  const channels = $derived.by<Channel[]>(() => {
    const map = new Map<string, Channel>();
    for (const b of bindings) {
      if (!b.profileUuid) {
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
          identity: destinationIdentityStore.forProfile(profile.uuid),
          connected: !!(provider && status?.connected),
          needsReconnect: !!provider && isStaleToken(status),
          armed: false,
          bindingUuids: [],
          streams: [],
        };
        map.set(accountId, ch);
      }
      ch.bindingUuids.push(b.uuid);
      if (b.enabled) {
        ch.armed = true;
      }
      if (!ch.streams.some((s) => s.profileUuid === b.profileUuid)) {
        // canvasNameFor, not forProfile: this row states which canvas the binding is
        // configured on, which a disarmed binding names just as well — resolving it
        // through the enabled-only profile lookup would print "no canvas" over exactly
        // the rows the user is reading to decide what to arm. Resolved live through
        // canvasStore either way, never the binding's rename-stale canvasName.
        ch.streams.push({
          profileUuid: b.profileUuid,
          profile,
          label: profile.label,
          canvasName: destinationIdentityStore.canvasNameFor(b.canvasUuid),
        });
      }
    }
    return [...map.values()];
  });
  // Only fully-connected channels are editable (prefill get + confirm set). A
  // needsReconnect channel is connected:false, so it is excluded here and still goes
  // live via its streams' keys.
  const connectedChannels = $derived(channels.filter((c) => c.connected));
  // Only ARMED connected channels take part in metadata traffic (prefill, push, save
  // chips, remember); a disarmed one renders as an inert card whose sole live control
  // is the arm switch. Counts (footer, reconnect strips) follow the armed subsets so
  // a switched-off destination is never reported as going live.
  const armedConnectedChannels = $derived(connectedChannels.filter((c) => c.armed));
  const armedChannels = $derived(channels.filter((c) => c.armed));

  // Modal width grows with the number of editable channel columns so a wide window
  // shows them side by side instead of one narrow stacked column, capped well short
  // of an ultrawide edge-to-edge span. COL_W mirrors the .chgrid minmax below so the
  // computed width actually fits that many columns per row; needsReconnect strips
  // span the full row and don't count toward the column budget.
  const COL_W: Record<"simple" | "advanced", number> = { simple: 340, advanced: 420 };
  const MIN_MODAL_W: Record<"simple" | "advanced", number> = { simple: 580, advanced: 760 };
  const MAX_MODAL_W = 1360;
  const GRID_GAP = 12;
  const BODY_PAD = 28; // Modal .modal-body left+right padding
  const modalWidth = $derived.by<number>(() => {
    const cols = Math.max(connectedChannels.length, 1);
    const w = COL_W[view] * cols + GRID_GAP * (cols - 1) + BODY_PAD;
    return Math.min(Math.max(w, MIN_MODAL_W[view]), MAX_MODAL_W);
  });

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
    for (const c of armedConnectedChannels) {
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
  // stream count across every armed channel — both drive the footer summary and reconnect
  // strips. Disarmed channels are excluded throughout: they won't start, so counting them
  // as "ready"/"streams" (or nagging to reconnect one) would misreport the go-live set.
  const reconnectChannels = $derived(armedChannels.filter((c) => c.needsReconnect));
  const channelStreamCount = $derived(armedChannels.reduce((n, c) => n + c.streams.length, 0));
  // Footer summary (mock: "3 channels · 5 streams · all ready"). When any channel needs a
  // relink, count ready vs. need-reconnect instead so the strips are accounted for. Falls
  // back to a key-only line when no account-linked channels are armed.
  const footerNote = $derived.by<string>(() => {
    if (armedChannels.length === 0) {
      return armedProfileCount > 0
        ? `${armedProfileCount} destination${armedProfileCount === 1 ? "" : "s"} via stream key`
        : "No destinations armed.";
    }
    const streams = streamsLabel(channelStreamCount);
    if (reconnectChannels.length > 0) {
      const ready = `${armedConnectedChannels.length} ready`;
      return `${ready} · ${streams} · ${reconnectChannels.length} need reconnect`;
    }
    const chans = `${armedChannels.length} channel${armedChannels.length === 1 ? "" : "s"}`;
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
      armedConnectedChannels.map(async (c) => {
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
        // Descriptor-supplied defaults are the floor under saved + live: they fill
        // only keys both left empty, so a field like YouTube's privacy always
        // resolves to a concrete, safe selection (remembered value wins) instead
        // of an unset "—" the backend would have to guess for.
        for (const f of c.provider?.fields ?? []) {
          if (f.default !== undefined && isEmptyVal(f.type, merged[f.key])) {
            merged[f.key] = f.default;
          }
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
    destinationIdentityStore.start();
    // Gate prefill on every data source being ready so connectedChannels is populated
    // before the best-effort get runs (whenReady starts each store). The canvas list is
    // in the gate because the card's canvas labels now resolve through it rather than
    // through each binding's own (rename-stale) snapshot.
    Promise.all([
      oauthStore.whenReady(),
      outputBindingStore.whenReady(),
      streamProfileStore.whenReady(),
      canvasStore.whenReady(),
      // The gate must settle for the modal to render at all, so a failed read still
      // has to produce a value — but "not live" is indistinguishable from a real
      // idle answer and picks the wrong primary action, so say so.
      obs.call("getStreamingState").catch((e) => {
        const msg = (e as Error).message;
        showToast("Couldn't read stream state: " + msg, msg);
        return { active: false };
      }),
    ]).then(([, , , , st]) => {
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
    // Header chips go "saving" for every armed channel while the pushes are in flight.
    // Building fresh from the armed set also clears any stale chip on a channel the
    // user has since disarmed.
    channelSaveState = Object.fromEntries(
      armedConnectedChannels.map((c) => [c.accountId, "saving"] as [string, SaveState]),
    );
    // A retry must not show last round's reason beside a fresh "Saving…" chip.
    channelSaveError = {};
    // One job per stream, ARMED channels only — a disarmed channel produces no
    // streamMeta.set (its bindings are disabled, so streaming.start won't start it
    // either) and therefore can never land in failedByChannel and block the go-live.
    // Each stream's effective fields merge the channel default with that stream's own
    // overrides. YouTube needs the per-profile call; Twitch/Kick applying the same
    // channel twice (no divergence) is idempotent. The stream layer applies ONLY when
    // its override toggle is on, so an orphaned overrides bag (toggle off) can never
    // diverge a live broadcast — the toggle is the sole authority.
    const jobs = armedConnectedChannels.flatMap((c) =>
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
          goingLive: goLiveModal.mode === "golive",
        }),
      ),
    );
    // Partial-failure tolerance: a failed metadata push never blocks going live. One
    // aggregate toast (showToast replaces, so per-destination toasts would clobber
    // each other) names the channel(s) in human terms, not raw API strings.
    const failed = results
      .map((r, i) =>
        r.status === "rejected" ? { job: jobs[i], reason: r.reason as BridgeError } : null,
      )
      .filter((x): x is { job: (typeof jobs)[number]; reason: BridgeError } => x !== null);
    // Collapse failures to distinct CHANNELS: two failing streams on one channel are one
    // destination, so the count, the singular/plural branch, and the header chip all agree.
    const failedByChannel = new Map<string, { name: string; reason: BridgeError }>();
    for (const f of failed) {
      if (!failedByChannel.has(f.job.channel.accountId)) {
        failedByChannel.set(f.job.channel.accountId, {
          name: f.job.channel.provider?.displayName || f.job.stream.label || "this platform",
          reason: f.reason,
        });
      }
    }
    channelSaveState = Object.fromEntries(
      armedConnectedChannels.map(
        (c) => [c.accountId, failedByChannel.has(c.accountId) ? "error" : "saved"] as [string, SaveState],
      ),
    );
    const goingLive = goLiveModal.mode === "golive";
    // The card strip is the authoritative failure surface — persistent, wraps, and
    // sits next to the arm switch that is the remedy. The toast below is only the
    // attention-getter: its visible line truncates and dies in 4s, and its second
    // arg surfaces solely as a hover title.
    // Prefer the decoded user message (the streamer-readable sentence, no method/
    // step prefixes) in the card; the diagnostic chain stays on message for the
    // toast's hover title.
    channelSaveError = Object.fromEntries(
      [...failedByChannel].map(([id, f]) => [
        id,
        f.reason?.userMessage ?? f.reason?.message ?? "metadata push failed",
      ]),
    );
    // Stream info is a precondition, not a courtesy: if any armed channel's metadata
    // push failed, going live would stream with stale/wrong title+category. Block the
    // start on ANY failure (all OAuth providers) and keep the modal open — the remedy
    // (disarm the destination) is the user's call, named in the card strip, never
    // auto-taken. Update-info mode has no start to block, so it only reports.
    if (failedByChannel.size > 0) {
      const fails = [...failedByChannel.values()];
      const names = fails.map((v) => v.name).join(", ");
      const reason = fails[0].reason?.message ?? "metadata push failed";
      const lead = goingLive ? "Not going live — couldn't update " : "Couldn't update ";
      if (fails.length === 1) {
        showToast(lead + fails[0].name + (goingLive ? "" : " stream info"), reason);
      } else {
        showToast(lead + fails.length + " destinations", names);
      }
      if (goingLive) {
        submitting = false;
        return;
      }
    }
    // Remember these details for next time — best-effort, fired without awaiting so a
    // slow or failing save never blocks going live. Armed channels only: a disarmed
    // channel's fields were locked this session, so its bags hold only the prefill
    // echo — persisting that would overwrite remembered values the user never touched.
    // One save per channel with its raw layers: the channel bag plus the channel's
    // COMPLETE stream set. Streams with an enabled, non-empty override carry their
    // bag; every other stream carries {} so the store clears any override toggled off
    // this session (otherwise it would resurrect and re-apply on the next go-live).
    if (remember) {
      void Promise.allSettled(
        armedConnectedChannels.map((c) => {
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
  width={modalWidth}
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
            {#if armedConnectedChannels.length > 1}<span class="eh-note">— across all {armedConnectedChannels.length} channels</span>{/if}
          </p>
          {#each sharedFields as f (f.key)}
            {@render fieldRow(f, sharedValues[f.key], (v) => setSharedField(f.key, v), {})}
          {/each}
        </div>
      {/if}

      <!-- Keyed on channels.length too: disarming every channel from inside this
           modal zeroes armedProfileCount, and the cards must stay visible so the
           user can re-arm — only a truly empty destination set shows the note. -->
      {#if armedProfileCount === 0 && channels.length === 0}
        <p class="note">No armed destinations. Enable a destination on a canvas to push stream information.</p>
      {:else}
        <!-- Per-channel cards: side-by-side columns on wide windows, one stacked
             column on narrow ones. auto-fit + minmax means no explicit breakpoint is
             needed — a track collapses to one column once it can't fit the minmax
             floor. Reconnect strips (below) span every column as a full-width banner. -->
        <div class="chgrid">
        {#each channels as c (c.accountId)}
          {#if c.connected && c.provider}
            <!-- Disarmed forces collapsed: the body never renders while off, so its
                 fields can't be edited (and won't be pushed). collapsed[] still holds
                 the user's own preference for when the channel is re-armed. -->
            {@const isCollapsed = !c.armed || !!collapsed[c.accountId]}
            {@const stt = channelSaveState[c.accountId]}
            <!-- The error chip restates the .cherr strip below verbatim (same failure,
                 already shown in full there), so it's suppressed once the strip is
                 showing; saving/saved have no strip and keep their chip. -->
            {@const showChip = !!stt && stt !== "idle" && !(stt === "error" && channelSaveError[c.accountId])}
            {@const name = c.identity?.displayName ?? c.login}
            <div class="ch" class:off={!c.armed}>
              <!-- Two rows. The arm switch cannot live inside the .chh collapse trigger
                   (an interactive element inside another), and wrapping both rows in the
                   trigger would make every click on the switch collapse the card too — so
                   row 1 alone is the trigger and the switch is row 2's own control. -->
              <!-- nb also stays off while the failure strip shows: the strip draws no
                   top border of its own, so the header keeps the separating line. -->
              <div class="chhrow" class:nb={isCollapsed && !channelSaveError[c.accountId]}>
                <button
                  type="button"
                  class="chh"
                  aria-expanded={!isCollapsed}
                  disabled={!c.armed}
                  onclick={() => toggleCollapsed(c.accountId)}
                >
                  <!-- Channel identity leads; the platform closes the row as a mark, not
                       a word. Two cards on the same platform are told apart by name and
                       avatar, and the name gets the row width the platform label ate. -->
                  <Avatar url={c.identity?.channelAvatarUrl ?? ""} {name} size={26} />
                  <span class="nm">{name}</span>
                  <PlatformMark platform={c.provider.id} size={18} />
                  <!-- Same box width as .sw.arm below, against the same right inset, so
                       the two rows close on one alignment line. -->
                  <span class="car">
                    {#if c.armed}<Icon name={isCollapsed ? "caret-right" : "caret-down"} size={13} />{/if}
                  </span>
                </button>
                <div class="chh-r2">
                  <span class="badges">
                    <span class="streams">{streamsLabel(c.streams.length)}</span>
                    {#if !c.armed}
                      <span class="offchip">Off</span>
                    {/if}
                    {#if showChip}
                      <span
                        class="st"
                        class:saving={stt === "saving"}
                        class:ok={stt === "saved"}
                        class:err={stt === "error"}
                      >
                        <span class="dot"></span>{SAVE_LABEL[stt]}
                      </span>
                    {/if}
                  </span>
                  <button
                    type="button"
                    class="sw arm"
                    class:on={c.armed}
                    role="switch"
                    aria-checked={c.armed}
                    disabled={submitting || !!armBusy[c.accountId]}
                    title={c.armed
                      ? "Armed — switch off to go live without this destination"
                      : "Off — switch on to include this destination"}
                    onclick={() => void toggleArmed(c)}
                  >
                    <i></i>
                  </button>
                </div>
              </div>

              {#if channelSaveError[c.accountId]}
                <!-- Outside the collapse body on purpose: a collapsed card — including
                     a disarmed, force-collapsed one — must still explain the failure;
                     the toast is transient and truncates. -->
                <div class="cherr">
                  <span class="cherr-reason">{channelSaveError[c.accountId]}</span>
                  {#if goLiveModal.mode === "golive"}
                    <span class="cherr-fix">Switch this destination off to go live without it.</span>
                  {/if}
                </div>
              {/if}

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
                            <span class="scanvas">{s.canvasName ?? "—"}</span>
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
          {:else if c.needsReconnect && c.armed}
            <!-- Stale-scope channel: still goes live via key, but can't push metadata
                 until relinked. Kept visible as a warn strip with a Reconnect action
                 (the shared OAuth connect flow), never silently dropped. Armed only —
                 a disarmed one won't start, so there is nothing to warn about (and
                 disabled bindings never surfaced a strip before either). -->
            <div class="ch warn">
              <div class="warnstrip">
                <Avatar url={c.status?.avatarUrl ?? ""} name={c.provider?.displayName || c.login} size={20} />
                <span class="msg">
                  <b>{c.login}</b> — reconnect to edit
                  {c.provider?.displayName ?? "this platform"} stream info
                </span>
                <button type="button" class="rbtn" disabled={reconnectBusy(c)} onclick={() => reconnect(c)}>Reconnect</button>
              </div>
            </div>
          {/if}
        {/each}
        </div>
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
  /* Body wrapper: no padding of its own (Modal's .modal-body owns the scroll + pad).
     Field inputs are capped to a readable width wherever they render -- the shared
     block, a channel column, or an advanced field -- so a wide modal (several
     channel columns) never stretches a text input edge-to-edge. */
  .mb {
    display: block;
  }
  .field :global(input.inp),
  .field :global(select.inp) {
    max-width: 460px;
  }

  /* Channel grid: side-by-side columns on wide windows, one stacked column on
     narrow ones. auto-fit + minmax needs no explicit breakpoint -- a track
     collapses to a single column once the available width can't fit the minmax
     floor, matching how ScenesDock/StatsDock/MonitorPage already do responsive
     card grids in this app. Advanced mode's columns carry more (platform-only
     fields, per-stream overrides) so they get a wider floor. */
  .chgrid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
    align-items: start;
    gap: 12px;
  }
  .mb.adv .chgrid {
    grid-template-columns: repeat(auto-fit, minmax(380px, 1fr));
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
    /* Caps at roughly two channel columns wide so the shared block stays readable
       instead of stretching edge-to-edge on a modal sized for 3+ channels. */
    max-width: 720px;
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

  /* Channel card: a grid item in .chgrid (spacing comes from the grid gap, not a
     margin). A needsReconnect warn strip has no per-channel fields to lay out
     side-by-side, so it spans every column as one full-width banner.
     --ch-pad-x is the one shared horizontal inset for every section (header,
     error strip, body) so they can't drift out of alignment with each other. */
  .ch {
    --ch-pad-x: 11px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
    min-width: 0;
  }
  .ch.warn {
    grid-column: 1 / -1;
    border-color: color-mix(in srgb, var(--color-warn) 45%, var(--color-border));
  }
  /* Header: identity + platform mark on row 1, badges + arm switch on row 2. The
     wrapper owns the header background and the divider under it, so both rows read as
     one bar and the card body/failure strip start on a single line. */
  .chhrow {
    display: flex;
    flex-direction: column;
    background: var(--color-surface-2);
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .chhrow.nb {
    border-bottom: 0;
  }
  /* Row 1, the collapse trigger: avatar + channel name take the width, the platform
     mark and the caret close the row. No bottom padding — row 2 owns the gap. */
  .chh {
    display: flex;
    align-items: center;
    gap: 9px;
    width: 100%;
    min-width: 0;
    height: auto;
    padding: 9px var(--ch-pad-x) 0;
    background: transparent;
    border: 0;
    text-align: left;
    cursor: pointer;
  }
  /* Row 2: badges left, arm switch right, on the same inset as row 1 so the badges
     start under the avatar and the switch ends under the caret. */
  .chh-r2 {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 9px;
    padding: 6px var(--ch-pad-x) 8px;
  }
  .badges {
    display: flex;
    align-items: center;
    gap: 5px;
    flex-wrap: wrap;
    min-width: 0;
  }
  .chh:disabled {
    cursor: default;
  }
  /* Disarmed: header content dims but the arm switch keeps full strength — it is the
     card's only live control while off, and dimming it would hide the way back on. */
  .ch.off .chh,
  .ch.off .badges {
    opacity: 0.55;
  }
  .offchip {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
    border: var(--border-weight) solid var(--color-border);
    padding: 1px 5px;
    flex: 0 0 auto;
  }

  /* Per-channel failure strip: the persistent reason (+ remedy when a go-live was
     blocked) — the sole error surface in the header once the redundant Failed chip is
     suppressed. overflow-wrap because provider reasons are raw API strings of
     arbitrary length. */
  .cherr {
    padding: 8px var(--ch-pad-x);
    background: color-mix(in srgb, var(--color-warn) 7%, var(--color-surface));
    font-size: 10.5px;
    line-height: 1.45;
    overflow-wrap: anywhere;
  }
  .cherr:not(:last-child) {
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .cherr-reason {
    color: var(--color-warn);
  }
  .cherr-fix {
    display: block;
    margin-top: 3px;
    color: var(--color-dim);
  }
  .nm {
    font-weight: 600;
    font-size: 13px;
    color: var(--color-text);
    flex: 1 1 auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .streams {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-dim);
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
    padding: 2px 6px;
    white-space: nowrap;
    flex: 0 0 auto;
  }
  .st {
    display: flex;
    align-items: center;
    gap: 5px;
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
  /* Reachable only if a failure ever lands without a reason string to put in the
     strip below; the chip is suppressed whenever that strip carries the detail. */
  .st.err {
    color: var(--color-warn);
  }
  .st .dot {
    width: 6px;
    height: 6px;
    background: currentColor;
    flex: 0 0 auto;
  }
  /* A real chevron in a box the width of .sw.arm below: at caret-glyph size beside an
     18px brand mark, a text ▾ read as a stray dot rather than an affordance. */
  .car {
    display: grid;
    place-items: center;
    width: 32px;
    color: var(--color-muted);
    flex: 0 0 auto;
  }
  .chb {
    padding: var(--ch-pad-x);
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
</style>
