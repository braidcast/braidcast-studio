<script lang="ts">
  import {
    obs,
    type BridgeError,
    type OAuthProvider,
    type OAuthProviderField,
    type OAuthStatus,
    type StreamMeta,
    type StreamProfileInfo,
  } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { goLiveModal, closeGoLiveModal } from "$lib/dialogs/golive/goLiveModalOpener.svelte";
  import {
    ALL_LAYER,
    inheritLayers,
    isEmptyVal,
    isPerDestination,
    normOpt,
    resolveRequiredEnum,
  } from "$lib/dialogs/golive/fieldValue";
  import { openOAuthConnect, isOAuthConnecting } from "$lib/dialogs/oauthConnectOpener.svelte";
  import { canvasStore } from "$lib/stores/canvasStore.svelte";
  import {
    destinationIdentityStore,
    type DestinationIdentity,
  } from "$lib/stores/destinationIdentityStore.svelte";
  import { goLivePref, setGoLivePref } from "$lib/stores/goLivePrefStore.svelte";
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

  // Likewise for a destination count, shared by the footer summary and the
  // hide-disabled bar.
  function destinationsLabel(n: number): string {
    return `${n} destination${n === 1 ? "" : "s"}`;
  }

  // Provider/status/binding/profile lists come from the shared stores (one source of
  // truth); `loaded` gates the modal until they + the live flag have settled.
  let providers = $derived(oauthStore.providers);
  let statuses = $derived(oauthStore.statuses);
  let bindings = $derived(outputBindingStore.bindings);
  let profiles = $derived(streamProfileStore.profiles);
  let loaded = $state(false);
  // Three states, not two: true, false, and `null` = the streaming state could not be
  // read. Collapsing null into false offers "Go Live now" to someone already live, so
  // the unread state blocks the primary action and renders as "—" instead.
  let isLive = $state<boolean | null>(null);
  let liveReadError = $state<string | null>(null);
  let liveRetrying = $state(false);
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

  // Inheritance layers, resolved inherit-layer -> channel -> stream by effectiveFields
  // (later wins, empties omitted). `layerValues` holds every layer BELOW the channel,
  // keyed the way fieldValue names them: one cross-provider bucket (`ALL_LAYER`, the mock
  // "Shared defaults" block) plus one bucket per provider id. Keyed buckets rather than a
  // second state object beside a global one, so which bucket a field reads is decided by
  // the descriptor's scope in one helper instead of by which variable a call site picked.
  let layerValues = $state<Record<string, Record<string, unknown>>>({});
  // Per-channel defaults, keyed by accountId then field key. An empty field with a layer
  // below inherits that layer's value; one without stands alone. Applied
  // to every stream in the channel unless the stream overrides it.
  let channelValues = $state<Record<string, Record<string, unknown>>>({});
  // Per-stream overrides, keyed by profileUuid. A filled key diverges that single
  // broadcast from its channel default; empty keys inherit the channel. Advanced
  // mode only — never written in Simple mode.
  let streamOverrides = $state<Record<string, Record<string, unknown>>>({});
  // Advanced-only UI state: which streams have their override field set expanded.
  // Toggling off clears that stream's overrides so it cleanly inherits again.
  let streamOverrideOn = $state<Record<string, boolean>>({});

  // Inherit-layer keys the user has edited by hand, as bucket + key — per bucket, since
  // the same field key lives in as many buckets as there are providers. Prefill must never
  // seed or diverge a key the user owns, otherwise an edit made while the (fired-not-awaited)
  // get/getSaved are in flight would be silently overridden by a stale live value.
  const touchedLayers = new Set<string>();
  function touchedKey(bucket: string, key: string): string {
    return bucket + "::" + key;
  }
  function writeLayer(bucket: string, key: string, val: unknown): void {
    layerValues[bucket] = { ...(layerValues[bucket] ?? {}), [key]: val };
  }
  function setLayerField(bucket: string, key: string, val: unknown): void {
    touchedLayers.add(touchedKey(bucket, key));
    writeLayer(bucket, key, val);
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
  // The keys that ADDRESS a destination rather than describe it. One lookup per provider,
  // shared by everything that has to tell a stream's address apart from a metadata
  // divergence — the two live in the same per-stream bag but are governed by different
  // rules.
  //
  // TWO sources, deliberately, because they answer different questions. `perDestination`
  // says a field RENDERS per stream (in Simple as well as Advanced) and so has to be a
  // declared field. `targetFieldKey` names the address key whether or not a field exists
  // for it — Facebook's Page is chosen on the destination and rendered nowhere here, yet it
  // rides in the same bag, and reading only the flag left this set empty for the one
  // provider that has an address. Collapsing either into the other silently unaddresses a
  // stream: a rendered address that is not the key, or a key with nothing to render.
  function targetKeys(p: OAuthProvider | null): Set<string> {
    const keys = new Set((p?.fields ?? []).filter(isPerDestination).map((f) => f.key));
    if (p?.targetFieldKey) {
      keys.add(p.targetFieldKey);
    }
    return keys;
  }
  // What a stream keeps when it is NOT overriding its channel: its address only.
  function addressOnly(p: OAuthProvider | null, bag: Record<string, unknown>): Record<string, unknown> {
    const keys = targetKeys(p);
    return Object.fromEntries(Object.entries(bag).filter(([k]) => keys.has(k)));
  }
  // Is this bag key holding a value? The descriptor type behind the key decides, since
  // "empty" differs by shape. A key the descriptor does NOT declare has no type to test
  // against — a provider's addressing claim is remembered in this bag without being a field
  // — so anything non-nullish under such a key counts as held: guessing a type would read
  // that claim as empty and let the next save drop it.
  function bagKeyHeld(p: OAuthProvider | null, key: string, v: unknown): boolean {
    const f = p?.fields.find((fd) => fd.key === key);
    return f ? !isEmptyVal(f.type, v) : v != null;
  }
  // The keys in this bag that genuinely diverge from the channel: everything HELD that is not
  // this stream's address. A key present but holding nothing is not a divergence, by the same
  // argument that an address is not one.
  //
  // The override switch and the override badge both read this, so the two cannot disagree —
  // they are answering one question, and a switch reading "on" beside "0 overrides" is a
  // contradiction the user can see.
  function overrideKeys(p: OAuthProvider | null, bag: Record<string, unknown>): string[] {
    const address = targetKeys(p);
    return Object.entries(bag)
      .filter(([k, v]) => !address.has(k) && bagKeyHeld(p, k, v))
      .map(([k]) => k);
  }
  function hasOverrides(p: OAuthProvider | null, bag: Record<string, unknown>): boolean {
    return overrideKeys(p, bag).length > 0;
  }
  function toggleStreamOverride(uuid: string, p: OAuthProvider | null): void {
    const on = !streamOverrideOn[uuid];
    streamOverrideOn[uuid] = on;
    if (!on) {
      // The switch governs metadata divergence, not addressing: dropping the whole bag
      // would leave the stream with no destination to post to.
      const kept = addressOnly(p, streamOverrides[uuid] ?? {});
      if (Object.keys(kept).length) {
        streamOverrides = { ...streamOverrides, [uuid]: kept };
      } else {
        delete streamOverrides[uuid];
        streamOverrides = { ...streamOverrides };
      }
    }
  }
  function streamOverrideCount(uuid: string, p: OAuthProvider): number {
    return overrideKeys(p, streamOverrides[uuid] ?? {}).length;
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

  // The nearest layer this field's channel control inherits from, or undefined when it has
  // none. One lookup, so the ghost, the grouping, the empty-means-inherit rule and the
  // push all name the same bucket.
  function inheritBucket(f: OAuthProviderField, providerId: string): string | undefined {
    return inheritLayers(f, providerId)[0];
  }
  // The value a channel holding nothing of its own inherits: the first layer that holds
  // one, walked in the helper's order.
  function inheritedValue(f: OAuthProviderField, providerId: string): unknown {
    for (const bucket of inheritLayers(f, providerId)) {
      const v = layerValues[bucket]?.[f.key];
      if (!isEmptyVal(f.type, v)) {
        return v;
      }
    }
    return undefined;
  }
  // What a control holding nothing of its own effectively stands on: the nearest layer that
  // holds a value, else what effectiveFields emits from there — for a required field, its
  // descriptor default, which goes out on Go Live whether or not any layer was ever filled.
  // Held once because three sites render it (the cue's wording, the option a select lands
  // on, the row's note) and a second copy is one of them naming a value never sent.
  function inheritedShownValue(f: OAuthProviderField, providerId: string): unknown {
    const v = inheritedValue(f, providerId);
    return isEmptyVal(f.type, v) ? resolveRequiredEnum(f, "") : v;
  }
  // What a control at `layer` is EFFECTIVELY showing: its own value, else what it falls back
  // to, in the same order effectiveFields pushes — a stream falls back to its channel first,
  // then to the field's scope layers. Anything reading the value on screen (the provider's
  // note for the current choice) resolves through here, so a row that displays an inherited
  // value describes that value rather than the empty state standing in for it.
  function shownValue(
    f: OAuthProviderField,
    value: unknown,
    layer: "channel" | "stream",
    accountId: string,
    providerId: string,
  ): unknown {
    if (!isEmptyVal(f.type, value)) {
      return value;
    }
    if (!inheritsBelow(f, layer, providerId)) {
      return undefined;
    }
    if (layer === "stream" && !isEmptyVal(f.type, channelValues[accountId]?.[f.key])) {
      return channelValues[accountId]?.[f.key];
    }
    // Resolved through the shared helper so a row displaying a descriptor default still
    // carries that value's note — for YouTube's privacy the note IS the point of the field,
    // and it would otherwise appear only once a layer fills.
    return inheritedShownValue(f, providerId);
  }

  // What the layer under a channel control is called wherever the UI names it. A provider
  // layer is named after the platform: calling it "shared" would claim a reach the value
  // deliberately does not have. It is named as the user's OWN default, because the bare
  // platform name reads as the platform's default instead — on privacy that misreading
  // looks like nothing is set while a real value is queued to go out.
  function inheritLabel(f: OAuthProviderField, p: OAuthProvider): string {
    return inheritBucket(f, p.id) === ALL_LAYER ? "shared" : "your " + p.displayName + " default";
  }

  // Human-readable inherited value: the text for the controls that print the cue, and the
  // "is anything inherited here?" signal for those that show it as an ordinary value
  // instead. An enum is named by its option LABEL rather than the raw descriptor value, so
  // wherever it does surface it reads as the option the user would pick, not as "public".
  //
  // Empty exactly when nothing is inherited — resolved through the same call effectiveFields
  // makes, so the cue appears exactly when a value is pushed and stays absent when the key
  // is omitted. Without it a required field reads as unset on a just-opened modal while its
  // descriptor default goes out on Go Live.
  function inheritedGhostText(f: OAuthProviderField, providerId: string): string {
    const held = inheritedShownValue(f, providerId);
    if (f.type === "tags" || f.type === "labelset") {
      return Array.isArray(held) ? held.join(", ") : "";
    }
    if (f.type === "category") {
      return held && typeof held === "object" ? ((held as { name?: string }).name ?? "") : "";
    }
    if (f.type === "enum") {
      const opt = (f.options ?? []).map(normOpt).find((o) => o.value === held);
      return opt?.label ?? (typeof held === "string" ? held : "");
    }
    return typeof held === "string" ? held : "";
  }

  // Field grouping (data lists, not branches): simple fields WITH a layer below render as
  // overrides of it (ghost/amber), simple fields without one render normally, advanced go
  // under the dashed "<Platform>-only" divider. Which group a field lands in follows the
  // helper's layer list, so a descriptor changing a field's scope regroups it with no edits
  // here — and a provider-scoped field renders exactly as a cross-provider one does, only
  // inheriting from a different bucket.
  //
  // Per-destination fields are excluded from all three at the one point below: they say
  // where a stream posts, so rendering them at the channel layer would offer a single
  // control for a value each stream sets differently. They render per stream instead.
  function channelFields(p: OAuthProvider): OAuthProviderField[] {
    return p.fields.filter((f) => !isPerDestination(f));
  }
  function targetFields(p: OAuthProvider): OAuthProviderField[] {
    return p.fields.filter(isPerDestination);
  }
  function simpleInherited(p: OAuthProvider): OAuthProviderField[] {
    return channelFields(p).filter((f) => f.tier !== "advanced" && inheritBucket(f, p.id) !== undefined);
  }
  function simpleStandalone(p: OAuthProvider): OAuthProviderField[] {
    return channelFields(p).filter((f) => f.tier !== "advanced" && inheritBucket(f, p.id) === undefined);
  }
  function advancedFields(p: OAuthProvider): OAuthProviderField[] {
    return channelFields(p).filter((f) => f.tier === "advanced");
  }

  // The provider's note for the value this control is SHOWING, rendered as the row's
  // hint — so it runs the same required-enum resolution the widget renders, or a field
  // still on its descriptor default would show a selection with no note under it.
  // Provider-declared data rather than a per-platform branch here, so a second platform
  // with a costly option is a capability entry and nothing in this file.
  function noteFor(
    f: OAuthProviderField,
    value: unknown,
    layer: "channel" | "stream",
    accountId: string,
    providerId: string,
  ): string | undefined {
    const held = shownValue(f, value, layer, accountId, providerId);
    const shown = resolveRequiredEnum(f, held, inheritsBelow(f, layer, providerId));
    return f.optionNotes?.[shown];
  }

  // The row's hint line. An override explanation and the provider's note for the value on
  // screen are both true at once, so they compose rather than one silently displacing the
  // other — which is how the quota warning went missing when a field gained a layer below.
  function hintFor(
    f: OAuthProviderField,
    value: unknown,
    layer: "channel" | "stream",
    accountId: string,
    providerId: string,
    overrideOf: string | null,
  ): string | undefined {
    const parts: string[] = [];
    if (overrideOf) {
      parts.push("Overrides the " + overrideOf + " " + f.label.toLowerCase() + " for this channel.");
    }
    const note = noteFor(f, value, layer, accountId, providerId);
    if (note) {
      parts.push(note);
    }
    return parts.length ? parts.join(" ") : undefined;
  }

  function isOverridden(id: string, f: OAuthProviderField): boolean {
    return !isEmptyVal(f.type, channelValues[id]?.[f.key]);
  }

  // Whether an empty value in this control means "inherit the layer below" rather than
  // "unset": the per-stream layer always falls back to the channel, and a channel layer
  // falls back to whichever bucket the field's scope names (none for a per-destination
  // field, which addresses this one stream — an empty control there means unaddressed).
  // A `required` field keeps its empty option at those sites — resolving it there would
  // manufacture an override the user never made, and pin a stream to a value diverging
  // from its channel.
  function inheritsBelow(f: OAuthProviderField, layer: "channel" | "stream", providerId: string): boolean {
    if (isPerDestination(f)) {
      return false;
    }
    return layer === "stream" || inheritBucket(f, providerId) !== undefined;
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

  // "Hide disabled" filters out the cards for channels whose output bindings are ALL
  // disabled. `armed` is not a separate state: it is set above purely from
  // `b.enabled`, the same per-binding flag the Multistream dock's row switch and this
  // card's arm switch both write through outputBindingStore.setEnabled. Restricted to
  // `connected` because that is the only branch rendering a card — a disarmed
  // reconnect strip is already suppressed and an unresolved channel renders nothing.
  // PRESENTATION ONLY: every armed-* set derives from `channels`, and each is already
  // a subset of `armed`, so this filter cannot change what is pushed or streamed.
  function isHiddenWhenDisabled(c: Channel): boolean {
    return !c.armed && c.connected;
  }
  const hiddenChannelCount = $derived(channels.filter(isHiddenWhenDisabled).length);
  const visibleChannels = $derived(
    goLivePref.hideDisabled ? channels.filter((c) => !isHiddenWhenDisabled(c)) : channels,
  );

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
    // Counts the columns actually rendered, so hiding disabled channels also gives
    // back the width they reserved.
    const cols = Math.max(visibleChannels.filter((c) => c.connected).length, 1);
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

  // Shared-defaults descriptor: the UNION of every field whose nearest layer is the
  // CROSS-PROVIDER bucket, across connected providers, deduped by key (first provider's
  // label/type wins). Drives the shared block so a provider widening a field's scope to
  // "all" gets a shared source with no edits here. A provider-scoped field is absent by
  // construction — its bucket belongs to one platform, so a block spanning every connected
  // channel is the wrong place to edit it.
  //
  // Every CONNECTED channel, not just the armed ones: a shared value belongs to no
  // destination, so switching them all off must not take the block — and the title and
  // description typed into it — off the screen with them.
  const sharedFields = $derived.by<OAuthProviderField[]>(() => {
    const seen = new Set<string>();
    const out: OAuthProviderField[] = [];
    for (const c of connectedChannels) {
      if (!c.provider) {
        continue;
      }
      for (const f of c.provider.fields) {
        if (inheritBucket(f, c.provider.id) === ALL_LAYER && !seen.has(f.key)) {
          seen.add(f.key);
          out.push(f);
        }
      }
    }
    return out;
  });

  // Edit mode names the action after the stream's state; with that state unread the
  // label stays neutral rather than asserting one. Pushing metadata is valid either
  // way, so only the go-live primary is actually blocked (see the footer gate).
  const primaryLabel = $derived(
    goLiveModal.mode === "golive"
      ? "Go Live now"
      : isLive === null
        ? "Apply info"
        : isLive
          ? "Update info"
          : "Save info",
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
        ? `${destinationsLabel(armedProfileCount)} via stream key`
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

  // Resolve effective values through the layers and push them: a stream override wins over
  // the channel default, which wins over the nearest inherit layer the field's scope names
  // (a field scoped to the channel has none). Empty fields are never emitted at any
  // layer, so a provider that treats "present" as "set" can't blank a channel by
  // inheriting nothing. `stream` omitted => the channel's own defaults, nothing else.
  //
  // Which of a stream's own values take part is one rule, held here: the override switch
  // governs the metadata a stream may diverge on, but a per-destination value is this
  // stream's ADDRESS, not a divergence — it applies switch or no switch, or two streams
  // meant for two Pages would both push the channel's one.
  function streamValue(f: OAuthProviderField, stream: Stream | undefined): unknown {
    if (!stream || (!streamOverrideOn[stream.profileUuid] && !isPerDestination(f))) {
      return undefined;
    }
    return streamOverrides[stream.profileUuid]?.[f.key];
  }
  function effectiveFields(c: Channel, stream: Stream | undefined): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    if (!c.provider) {
      return out;
    }
    const cv = channelValues[c.accountId] ?? {};
    for (const f of c.provider.fields) {
      const sv = streamValue(f, stream);
      const inherited = inheritedValue(f, c.provider.id);
      if (!isEmptyVal(f.type, sv)) {
        out[f.key] = sv;
      } else if (!isEmptyVal(f.type, cv[f.key])) {
        out[f.key] = cv[f.key];
      } else if (!isEmptyVal(f.type, inherited)) {
        out[f.key] = inherited;
      } else {
        // Nothing held at any layer. A `required` field has no valid empty state, so
        // emit the value its control is showing — the descriptor default — rather than
        // omit the key and let the provider substitute something else.
        const shown = resolveRequiredEnum(f, "");
        if (shown !== "") {
          out[f.key] = shown;
        }
      }
    }
    // An address key with no descriptor field is unreachable by the loop above, which walks
    // fields. Facebook's Page is exactly that: claimed on the destination, rendered nowhere
    // here, yet the provider resolves which destination to post to from this key alone and
    // refuses the push outright when an account administers more than one. It belongs to the
    // stream, so it is emitted only for one — the channel's own bag addresses nothing.
    if (stream) {
      const declared = new Set(c.provider.fields.map((f) => f.key));
      for (const key of targetKeys(c.provider)) {
        const v = streamOverrides[stream.profileUuid]?.[key];
        if (!declared.has(key) && bagKeyHeld(c.provider, key, v)) {
          out[key] = v;
        }
      }
    }
    return out;
  }

  // Every key streamMeta.get reports, each with the descriptor type its value carries.
  // StreamMeta is a closed struct, so the live read is consumed by walking this list rather
  // than by one hand-written lift per key — three such lifts silently left Kick's live tags
  // on the floor, and a fourth would have been the same omission waiting to happen. A key
  // added to the struct and not to this list fails the `keyof StreamMeta` check.
  //
  // The type is what makes each lift correct: emptiness is per type, so a provider reporting
  // an unset category as a blank id is skipped by the same rule that skips a blank title.
  const LIVE_META_FIELDS: { key: keyof StreamMeta; type: string }[] = [
    { key: "title", type: "text" },
    { key: "category", type: "category" },
    { key: "language", type: "enum" },
    { key: "tags", type: "tags" },
  ];

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

        // Channel bag: saved base, live over — a provider reporting a value now outranks the
        // one remembered from last time, and an empty live read leaves the remembered one
        // standing rather than blanking it.
        const merged: Record<string, unknown> = { ...saved.channel };
        for (const { key, type } of LIVE_META_FIELDS) {
          const v = live?.[key];
          if (!isEmptyVal(type, v)) {
            merged[key] = v;
          }
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
        // Restore remembered per-stream values first, so the routing below only fills
        // what a stream has nothing of. Skip a stream the user has already touched. The
        // override SWITCH follows the bag's content, not its mere presence: a bag holding
        // only this stream's address is not a divergence from the channel, and flipping
        // the switch for it would report overrides the user never made.
        for (const [uuid, bag] of Object.entries(saved.streams)) {
          if (bag && Object.keys(bag).length && !streamOverrideOn[uuid] && !streamOverrides[uuid]) {
            streamOverrides[uuid] = { ...bag };
            streamOverrideOn[uuid] = hasOverrides(c.provider, bag);
          }
        }

        // Route each merged key to the layer its scope names, so an inheritable value lands
        // in that INHERIT LAYER (first channel wins), not as a spurious per-channel
        // override: a channel only takes an override when it genuinely diverges from the
        // layer. Without this, two channels live-reporting the same title would each read as
        // "overrides shared". This is also what makes a provider-scoped value reach a second
        // channel of the same platform — a thumbnail or category entered once on one YouTube
        // channel is remembered, restored here, and inherited by the other. A channel-scoped
        // key stands alone in channelValues, EXCEPT a per-destination key, which addresses a
        // stream and so seeds the streams that have no address yet. Every write is guarded so
        // a user edit before this resolves is never clobbered.
        for (const [key, val] of Object.entries(merged)) {
          const f = c.provider?.fields.find((fd) => fd.key === key);
          const type = f?.type ?? "text";
          if (isEmptyVal(type, val)) {
            continue;
          }
          const bucket = f && c.provider ? inheritBucket(f, c.provider.id) : undefined;
          if (f && isPerDestination(f)) {
            for (const s of c.streams) {
              if (isEmptyVal(type, getStreamVal(s.profileUuid, key))) {
                setStreamField(s.profileUuid, key, val);
              }
            }
          } else if (bucket) {
            if (touchedLayers.has(touchedKey(bucket, key))) {
              continue;
            }
            const held = layerValues[bucket]?.[key];
            if (isEmptyVal(type, held)) {
              writeLayer(bucket, key, val);
            } else if (!valuesEqual(type, held, val) && isEmptyVal(type, getVal(c.accountId, key))) {
              setField(c.accountId, key, val);
            }
          } else if (isEmptyVal(type, getVal(c.accountId, key))) {
            setField(c.accountId, key, val);
          }
        }
      }),
    );
  }

  // The one streaming-state reader, shared by the open gate and the retry affordance.
  // A rejection resolves to `active: null` (unread) carrying the reason, never to a
  // fabricated idle answer.
  async function readLiveState(): Promise<{ active: boolean | null; error: string | null }> {
    try {
      const st = await obs.call("getStreamingState");
      return { active: st.active, error: null };
    } catch (e) {
      return { active: null, error: (e as Error).message };
    }
  }

  async function retryLiveState(): Promise<void> {
    if (liveRetrying) {
      return;
    }
    liveRetrying = true;
    const st = await readLiveState();
    isLive = st.active;
    liveReadError = st.error;
    liveRetrying = false;
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
      // has to produce a result — but it resolves to "unread", never to "not live":
      // that guess picks the wrong primary action for the whole modal.
      readLiveState(),
    ]).then(([, , , , st]) => {
      if (!active) {
        return;
      }
      isLive = st.active;
      liveReadError = st.error;
      loaded = true;
      if (st.error) {
        showToast("Couldn't read stream state: " + st.error, st.error);
      }
      void prefill();
    });
    const off = obs.on(EV.streamingChanged, (p) => {
      isLive = p.active;
      liveReadError = null;
    });
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
    // values. YouTube needs the per-profile call; Twitch/Kick applying the same channel
    // twice (no divergence) is idempotent. Which of a stream's values apply is decided
    // in one place (streamValue): the override toggle is the sole authority over
    // metadata divergence, while a per-destination address always applies.
    const jobs = armedConnectedChannels.flatMap((c) =>
      c.streams.map((s) => ({ channel: c, stream: s, fields: effectiveFields(c, s) })),
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
    // A push that landed recorded itself against a go-live, and the host holds that
    // record until the session opens and consumes it. Every way out of this function
    // that leaves no session coming has to hand those records back, or the host keeps
    // reading them as "this channel was already told" and skips its metadata push on
    // every later go-live — including a scheduled one, which has no modal to push or
    // record for it. Both early returns below call this; a third would have to as well.
    const forgetPushedMetadata = (): void => {
      const pushed = jobs
        .filter((_, i) => results[i].status === "fulfilled")
        .map((j) => j.stream.profileUuid);
      if (pushed.length > 0) {
        void obs.call("streamMeta.forgetSent", { profileUuids: pushed }).catch(() => {});
      }
    };
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
        forgetPushedMetadata();
        submitting = false;
        return;
      }
    }
    // Remember these details for next time — best-effort, fired without awaiting so a
    // slow or failing save never blocks going live. Armed channels only: a disarmed
    // channel's fields were locked this session, so its bags hold only the prefill
    // echo — persisting that would overwrite remembered values the user never touched.
    // One save per channel with its raw layers: the channel bag plus the channel's
    // COMPLETE stream set. A stream with its override switch on carries its whole bag;
    // one with the switch off carries only its per-destination address, so the store
    // clears the divergences toggled off this session (they would otherwise resurrect
    // and re-apply on the next go-live) WITHOUT unaddressing the stream. An empty result
    // clears the entry outright, which is what a channel with no addressing wants.
    if (remember) {
      void Promise.allSettled(
        armedConnectedChannels.map((c) => {
          const streams: Record<string, Record<string, unknown>> = {};
          for (const s of c.streams) {
            const ov = streamOverrides[s.profileUuid] ?? {};
            streams[s.profileUuid] = streamOverrideOn[s.profileUuid] ? ov : addressOnly(c.provider, ov);
          }
          return obs.call("streamMeta.save", {
            accountId: c.accountId,
            // Persist the EFFECTIVE channel-level defaults (every inherit layer merged
            // with channel values, empties dropped) so inherited values — which live in
            // layerValues, not channelValues — are actually saved. Prefill re-derives the
            // layers from them on read (first-wins + dedup), so neither the persisted
            // shape nor the store needs to know layers exist.
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
        // Nothing this modal armed is going live, so the records go back. They go back
        // even in the case this catch was written for — a start refused because an
        // EARLIER go-live's prelude is still running — where that prelude may yet land
        // and open a session of its own. Handing them over costs that session its title
        // in the history card; keeping them costs a leak whenever the earlier prelude
        // is refused instead, since it only takes back what it recorded itself, at most
        // one profile per persistent channel. The cheaper loss wins.
        forgetPushedMetadata();
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
    accountId?: string;
    ghostText?: string;
    ghostValue?: unknown;
    accent?: boolean;
    narrow?: boolean;
    hint?: string;
    tag?: string;
    inheritable?: boolean;
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
        accountId={opts.accountId ?? ""}
        ghostText={opts.ghostText ?? ""}
        ghostValue={opts.ghostValue}
        accent={opts.accent ?? false}
        narrow={opts.narrow ?? false}
        inheritable={opts.inheritable ?? false}
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
    {#if isLive === null}
      <span class="live-unknown" title={liveReadError ?? "Stream state unavailable"}>—</span>
    {:else if isLive}
      <span class="live-dot" title="Live"></span>
    {/if}
    <Segmented options={VIEW_OPTIONS} value={view} onChange={(v) => (view = v as "simple" | "advanced")} />
  {/snippet}

  <div class="mb" class:adv={view === "advanced"}>
    {#if !loaded}
      <p class="note">Loading destinations…</p>
    {:else}
      {#if isLive === null}
        <!-- Unread streaming state. In golive mode the primary action is held rather
             than offering a start that could double up on a stream already running;
             in edit mode only the live indicator is unknown, so the push stays open. -->
        <div class="statewarn">
          <span class="msg">
            Stream state unavailable{liveReadError ? " — " + liveReadError : ""}.
            {goLiveModal.mode === "golive"
              ? "Going live could start a second stream over one already running, so Go Live is held until this reads."
              : "Stream information still applies; only the live indicator is unknown."}
          </span>
          <button type="button" class="rbtn" disabled={liveRetrying} onclick={() => void retryLiveState()}>
            {liveRetrying ? "Retrying…" : "Retry"}
          </button>
        </div>
      {/if}

      <!-- Shared defaults: the union of connected providers' cross-provider-scoped fields
           (mock `.shared`), one row each, present in both modes whenever anything is. -->
      {#if sharedFields.length}
        <div class="shared">
          <p class="eh">
            Shared defaults
            {#if armedConnectedChannels.length > 1}<span class="eh-note">— across all {armedConnectedChannels.length} channels</span>{/if}
          </p>
          {#each sharedFields as f (f.key)}
            {@render fieldRow(f, layerValues[ALL_LAYER]?.[f.key], (v) => setLayerField(ALL_LAYER, f.key, v), {})}
          {/each}
        </div>
      {/if}

      <!-- Keyed on channels.length too: disarming every channel from inside this
           modal zeroes armedProfileCount, and the cards must stay visible so the
           user can re-arm — only a truly empty destination set shows the note. -->
      {#if armedProfileCount === 0 && channels.length === 0}
        <p class="note">No armed destinations. Enable a destination on a canvas to push stream information.</p>
      {:else}
        <!-- Hide-disabled bar. Shown whenever any destination is switched off, in both
             positions of the toggle, so the default (hiding) can never look like the
             modal lost a destination — the count states what is missing and the switch
             is the way back to it. Revealed cards stay fully interactive: their arm
             switch re-enables the binding in place, after which the card is armed and
             no longer filtered. -->
        {#if hiddenChannelCount > 0}
          <div class="viewbar">
            <button
              type="button"
              class="sw"
              class:on={goLivePref.hideDisabled}
              role="switch"
              aria-checked={goLivePref.hideDisabled}
              title={goLivePref.hideDisabled
                ? "Show disabled destinations"
                : "Hide disabled destinations"}
              onclick={() => setGoLivePref("hideDisabled", !goLivePref.hideDisabled)}
            >
              <i></i>
            </button>
            <span class="vbl">Hide disabled</span>
            <span class="vbc">
              {destinationsLabel(hiddenChannelCount)} disabled{goLivePref.hideDisabled
                ? " and hidden — switch this off to show and re-enable them"
                : " shown below"}
            </span>
          </div>
        {/if}

        <!-- Per-channel cards: side-by-side columns on wide windows, one stacked
             column on narrow ones. auto-fit + minmax means no explicit breakpoint is
             needed — a track collapses to one column once it can't fit the minmax
             floor. Reconnect strips (below) span every column as a full-width banner. -->
        <div class="chgrid">
        {#each visibleChannels as c (c.accountId)}
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
                {@const targets = targetFields(c.provider)}
                <div class="chb">
                  {#if c.streams.length > 1}
                    <!-- Two different truths, told apart by the descriptor rather than by
                         platform: an account that is one channel really does publish every
                         stream to the same place, while an account holding several targets
                         (Facebook Pages) has each stream landing somewhere else on purpose. -->
                    <div class="dedupe">
                      {#if targets.length}
                        Each of these <b>{c.streams.length} streams</b> posts to its own
                        {targets.map((f) => f.label.toLowerCase()).join(" / ")} — chosen per stream below. Everything
                        else on this card applies to all of them.
                      {:else}
                        All <b>{c.streams.length} streams</b> post to this one channel — its title, category and
                        thumbnail are set once here.
                      {/if}
                    </div>
                  {/if}

                  {#if targets.length}
                    <!-- Where each stream posts. Shown in Simple as well as Advanced: this
                         is the destination itself, not an optional divergence from the
                         channel, so burying it would leave a stream addressed by a value
                         the user never saw. -->
                    <div class="targets">
                      {#each c.streams as s (s.profileUuid)}
                        <div class="trow">
                          <div class="trh">
                            <span class="tn">{s.label}</span>
                            <span class="tc">{s.canvasName ?? "—"}</span>
                          </div>
                          {#each targets as f (f.key)}
                            {@render fieldRow(f, getStreamVal(s.profileUuid, f.key), (v) => setStreamField(s.profileUuid, f.key, v), {
                              providerId: c.provider.id,
                              accountId: c.accountId,
                              hint: noteFor(
                                f,
                                getStreamVal(s.profileUuid, f.key),
                                "stream",
                                c.accountId,
                                c.provider.id,
                              ),
                            })}
                          {/each}
                        </div>
                      {/each}
                    </div>
                  {/if}

                  <!-- Fields with a layer below, as per-channel overrides of it. While
                       inheriting, the control shows the inherited value the way a chosen one
                       looks and the tag alone says it is inherited; amber marks a channel
                       that diverges. The tag names the layer, so a value shared only among
                       this platform's channels never reads as one shared with every
                       platform. -->
                  {#each simpleInherited(c.provider) as f (f.key)}
                    {@const filled = isOverridden(c.accountId, f)}
                    {@const layer = inheritLabel(f, c.provider)}
                    {@render fieldRow(f, getVal(c.accountId, f.key), (v) => setField(c.accountId, f.key, v), {
                      providerId: c.provider.id,
                      accountId: c.accountId,
                      ghostText: inheritedGhostText(f, c.provider.id),
                      ghostValue: inheritedShownValue(f, c.provider.id),
                      inheritable: inheritsBelow(f, "channel", c.provider.id),
                      accent: filled,
                      tag: filled ? "— overrides " + layer : "↳ using " + layer,
                      hint: hintFor(
                        f,
                        getVal(c.accountId, f.key),
                        "channel",
                        c.accountId,
                        c.provider.id,
                        filled ? layer : null,
                      ),
                    })}
                  {/each}

                  <!-- Channel-scoped fields (privacy / language / …), one per row. -->
                  {#each simpleStandalone(c.provider) as f (f.key)}
                    {@render fieldRow(f, getVal(c.accountId, f.key), (v) => setField(c.accountId, f.key, v), {
                      providerId: c.provider.id,
                      accountId: c.accountId,
                      hint: noteFor(f, getVal(c.accountId, f.key), "channel", c.accountId, c.provider.id),
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
                            accountId: c.accountId,
                            narrow: f.type === "enum",
                            inheritable: inheritsBelow(f, "channel", c.provider.id),
                            hint: noteFor(f, getVal(c.accountId, f.key), "channel", c.accountId, c.provider.id),
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
                                onclick={() => toggleStreamOverride(s.profileUuid, c.provider)}
                              >
                                <i></i>
                              </button>
                            </span>
                          </div>
                          {#if on}
                            <div class="srb">
                              {#each channelFields(c.provider) as f (f.key)}
                                {@render fieldRow(f, getStreamVal(s.profileUuid, f.key), (v) => setStreamField(s.profileUuid, f.key, v), {
                                  providerId: c.provider.id,
                                  accountId: c.accountId,
                                  narrow: f.type === "enum",
                                  inheritable: inheritsBelow(f, "stream", c.provider.id),
                                  hint: noteFor(
                                    f,
                                    getStreamVal(s.profileUuid, f.key),
                                    "stream",
                                    c.accountId,
                                    c.provider.id,
                                  ),
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
    <button
      class="accent"
      disabled={submitting ||
        !loaded ||
        armedProfileCount === 0 ||
        (goLiveModal.mode === "golive" && isLive === null)}
      onclick={() => void confirm()}
    >
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
  /* Unread streaming state, rendered as the project's absent-value em-dash rather
     than as an off indicator. */
  .live-unknown {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 12px;
    color: var(--color-warn);
  }
  .note {
    font-size: 11px;
    color: var(--color-muted);
    margin: 0;
  }
  /* Unread-state banner: same treatment as the per-channel reconnect strip, spanning
     the modal body above the channel grid. */
  .statewarn {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 9px 11px;
    margin-bottom: 12px;
    border: var(--border-weight) solid color-mix(in srgb, var(--color-warn) 45%, var(--color-border));
    background: color-mix(in srgb, var(--color-warn) 7%, var(--color-surface));
  }
  .statewarn .msg {
    font-size: 11px;
    line-height: 1.45;
    color: var(--color-warn);
    overflow-wrap: anywhere;
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

  /* Hide-disabled bar: the switch + label idiom the savebar already uses, in a
     compact row above the channel grid. */
  .viewbar {
    display: flex;
    align-items: center;
    gap: 9px;
    flex-wrap: wrap;
    margin-bottom: 10px;
  }
  .vbl {
    font-size: 11px;
    color: var(--color-dim);
  }
  .vbc {
    font-size: 10px;
    color: var(--color-muted);
    min-width: 0;
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

  /* Per-stream destination pickers: one block per stream, named by its profile and
     canvas so two rows carrying the same control are told apart. */
  .targets {
    margin: 0 0 11px;
  }
  .trow {
    padding: 8px 9px;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
  }
  .trow + .trow {
    margin-top: 6px;
  }
  .trh {
    display: flex;
    align-items: baseline;
    gap: 8px;
    margin-bottom: 6px;
    min-width: 0;
  }
  .tn {
    font-size: 11px;
    font-weight: 600;
    color: var(--color-text);
    flex: 0 0 auto;
  }
  .tc {
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--color-muted);
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
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
  .rbtn:hover:not(:disabled) {
    background: color-mix(in srgb, var(--color-warn) 14%, transparent);
    border-color: var(--color-warn);
  }
  .rbtn:disabled {
    opacity: 0.5;
    cursor: default;
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
