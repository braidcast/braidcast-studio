// The one answer to "given a profileUuid, which live destination is this?".
//
// A destination is named to the user as CHANNEL + CANVAS, not by its stream profile
// label: the label is user-chosen text that mostly restates the channel, while the
// canvas is what actually differs between two broadcasts of one channel. Stats already
// renders `profileLabel > canvasName` and the Multistream dock groups by canvas, so
// canvas name keeps one vocabulary across the app.
//
// (channel, canvas) is a unique key because the engine refuses to enable one profile on
// a second canvas -- one RTMP key is one live stream (bridge.cpp, outputBinding.setEnabled).
// Two orientations of one channel are therefore two distinct profiles, not one profile
// on two canvases, which is exactly why "YouTube" alone stopped being an answer.
//
// Its own module rather than a method on one of the four stores it reads: the join spans
// stream profiles, output bindings, canvases and OAuth accounts, so hanging it off any
// one of them would make that entity depend on three siblings. It owns no bridge
// subscription of its own -- every field is derived from stores that already have one --
// so there is nothing here to fall out of sync.

import type { OutputBindingInfo, StreamProfileInfo } from "$lib/api/bridge";
import { canvasStore } from "$lib/stores/canvasStore.svelte";
import { channelsStore, type ChannelRow } from "$lib/stores/channelsStore.svelte";
import { oauthStore } from "$lib/stores/oauthStore.svelte";
import { outputBindingStore } from "$lib/stores/outputBindingStore.svelte";
import { streamProfileStore } from "$lib/stores/streamProfileStore.svelte";
import { platformKey } from "$lib/theme/platformColors";

export interface DestinationIdentity {
  profileUuid: string;
  /** The raw label from streams.json -- NOT the host's DisplayName(), which prefixes it
   * with the platform ("YouTube - Anime Cruizer") and would double up with the mark. */
  profileLabel: string;
  /** platformColors.ts key ("youtube" | "twitch" | "kick"); a non-OAuth profile yields
   * whatever its service word normalizes to ("rtmp", "whip"), which has no brand mark
   * and renders as the neutral one. */
  platform: string;
  /** Linked OAuth account ("providerId:userId"); "" for key/RTMP/WHIP profiles. */
  accountId: string;
  /** What this destination is: its claimed target when it has one (the Facebook Page),
   * else the linked account's display name. "" when there is no account, or before
   * oauth.status has loaded. Read `displayName` to print something. */
  channelName: string;
  /** Picture for the same thing `channelName` names -- the target's, else the
   * account's. Never the account's when a target picture exists. */
  channelAvatarUrl: string;
  /** The account holds a live token. False for needs-reconnect and for no account. */
  channelConnected: boolean;
  /** What to print: the channel, else the profile label, else a literal. ONE fallback
   * chain, so no two docks can invent different ones. */
  displayName: string;
  /** Canvas the profile's enabled binding points at; null when nothing is enabled --
   * the destination is armed nowhere. */
  canvasUuid: string | null;
  /** Resolved canvas name, or null for an explicit absence the caller renders as
   * "channel-wide" / "—". Never guessed, and never "the first canvas": a fabricated
   * canvas label is the bug this whole surface exists to remove. Null with a non-null
   * `canvasUuid` means the canvas list has not loaded yet, or the canvas is gone. */
  canvasName: string | null;
  /** The canvas's permanent number, and the encode size the canvas mark reads its
   * orientation from. 0 wherever `canvasName` is null -- same absence, so a caller
   * that has already decided not to name a canvas needs no second test. Carried here
   * rather than re-joined per row: this store already holds the canvas object.  */
  canvasNumber: number;
  canvasWidth: number;
  canvasHeight: number;
  /** The enabled binding, for joining multistreamStatusStore.statusByBinding without
   * re-deriving that store's state reduction here. */
  bindingUuid: string | null;
  /** True when the profile has at least one binding CONFIGURED but none of them enabled:
   * it is wired to a canvas and merely switched off. Only meaningful while `canvasUuid` is
   * null, which cannot distinguish the two on its own. They are worth telling apart
   * because they ask for opposite actions -- enable the binding that exists, versus go
   * create one -- and because a destination bound to two canvases reading "not armed"
   * contradicts what the Multistream dock shows for the same rows. */
  boundButDisabled: boolean;
}

/** What a destination with no enabled binding is called. Only meaningful while
 * `canvasUuid` is null.
 *
 * Two words rather than one, for the reason `boundButDisabled` exists: a binding
 * merely switched off asks the user to enable it, while no binding at all asks them
 * to go create one, and a single label sends half of them to the wrong page.
 * "disabled" is the Multistream dock's own word for these rows -- one state must not
 * read as two different problems across the destination chips, the dock and the
 * schedule editor, which is exactly what three hand-written copies produce. */
export function unarmedLabel(d: DestinationIdentity): string {
  return d.boundButDisabled ? "disabled" : "not armed";
}

class DestinationIdentityStore {
  #started = false;

  #profileByUuid = $derived.by<Map<string, StreamProfileInfo>>(
    () => new Map(streamProfileStore.profiles.map((p) => [p.uuid, p])),
  );

  // profileUuid -> its enabled binding. First match is total, not arbitrary: the engine
  // permits at most one enabled binding per profile (see the file header).
  #enabledBindingByProfile = $derived.by<Map<string, OutputBindingInfo>>(() => {
    const m = new Map<string, OutputBindingInfo>();
    for (const b of outputBindingStore.bindings) {
      if (b.enabled && b.profileUuid && !m.has(b.profileUuid)) {
        m.set(b.profileUuid, b);
      }
    }
    return m;
  });

  // Profiles with any binding at all, enabled or not. Deliberately NOT folded into
  // #enabledBindingByProfile: that map's whole contract is that it only ever sees enabled
  // bindings, and widening it would make every reader of it suspect.
  #boundProfiles = $derived.by<Set<string>>(() => {
    const s = new Set<string>();
    for (const b of outputBindingStore.bindings) {
      if (b.profileUuid) {
        s.add(b.profileUuid);
      }
    }
    return s;
  });

  #channelByAccount = $derived.by<Map<string, ChannelRow>>(
    () => new Map(channelsStore.rows.map((r) => [r.accountId, r])),
  );

  /** Every stream profile as a destination, in streamProfileStore order. */
  readonly all = $derived.by<DestinationIdentity[]>(() =>
    streamProfileStore.profiles.map((p) => this.#identify(p)),
  );

  /** Idempotent; the first consumer to mount starts the stores this joins. */
  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    streamProfileStore.start();
    outputBindingStore.start();
    canvasStore.start();
    // channelsStore.rows only carries identity once oauth.status has loaded. App.svelte
    // holds that app-wide, but a detached dock window mounts docks without it -- so take
    // a ref-counted subscription of our own (never released, matching the start()s
    // above) rather than let a detached Chat/Events dock show nameless channels.
    void oauthStore.subscribe();
  }

  /** The three uuid-keyed stores have settled. Channel identity is not gated on this:
   * it fills in as oauth.status arrives, degrading to the profile label meanwhile. */
  get loaded(): boolean {
    return streamProfileStore.loaded && outputBindingStore.loaded && canvasStore.loaded;
  }

  /** Null for an unknown profileUuid -- an explicit absence, not an empty identity. */
  forProfile(profileUuid: string): DestinationIdentity | null {
    const profile = this.#profileByUuid.get(profileUuid);
    return profile ? this.#identify(profile) : null;
  }

  /**
   * Current name of a canvas, or null when no canvas holds that uuid.
   *
   * The configuration counterpart to forProfile(), and NOT a redundant second way to
   * ask the same thing -- resist collapsing them. forProfile() answers "which canvas is
   * this destination LIVE on", so it consults only the ENABLED binding and its null
   * genuinely means unknowable ("channel-wide"). This answers "which canvas is this
   * binding CONFIGURED on", where a binding names its canvas whether armed or not, so
   * "channel-wide" is not a reachable state and null means only "no such canvas". Route
   * a configuration surface through forProfile() and every disarmed row inside an armed
   * channel claims to have no canvas -- hiding the one fact the user needs in order to
   * decide what to arm.
   *
   * Canvas-scoped rather than binding-scoped deliberately: taking a canvasUuid makes it
   * structurally impossible for this path to consult enabled state, and keeps the shape
   * unmistakably unlike forProfile()'s so the two never read as interchangeable.
   *
   * Resolves through canvasStore, never a binding's `canvasName` snapshot: a rename
   * fires canvas.changed, which outputBindingStore does not listen for, so that field
   * goes stale while the canvas list cannot.
   */
  canvasNameFor(canvasUuid: string): string | null {
    if (!canvasUuid) {
      return null;
    }
    return canvasStore.byUuid(canvasUuid)?.name ?? null;
  }

  #identify(p: StreamProfileInfo): DestinationIdentity {
    const channel = p.accountId ? this.#channelByAccount.get(p.accountId) : undefined;
    const binding = this.#enabledBindingByProfile.get(p.uuid);
    // Resolve the name through canvasStore rather than the binding's canvasName
    // snapshot: a rename pushes canvas.changed, which outputBindingStore does not
    // listen for, so that snapshot can go stale while the canvas list cannot.
    const canvas = binding ? canvasStore.byUuid(binding.canvasUuid) : undefined;
    // The claimed target outranks the account, and must: several profiles can share one
    // account, so the account's name and picture are identical across all of them and
    // name the person rather than where the stream lands. Only the target tells two
    // Facebook Pages apart. Providers whose account IS the destination send no target,
    // so they fall through to the account exactly as before.
    const targetName = p.targetName.trim();
    const channelName = targetName || (channel?.displayName.trim() ?? "");
    const profileLabel = p.label.trim();
    return {
      profileUuid: p.uuid,
      profileLabel,
      // The account's providerId is the authoritative key; StreamProfileInfo.platform is
      // a display prefix the host derives from the service, so it is only the fallback.
      platform: channel?.providerId ?? platformKey(p.platform),
      accountId: p.accountId,
      channelName,
      channelAvatarUrl: p.targetAvatarUrl.trim() || (channel?.avatarUrl ?? ""),
      channelConnected: channel?.connected ?? false,
      displayName: channelName || profileLabel || "Untitled destination",
      canvasUuid: binding?.canvasUuid ?? null,
      canvasName: canvas?.name ?? null,
      canvasNumber: canvas?.number ?? 0,
      canvasWidth: canvas?.outputWidth ?? 0,
      canvasHeight: canvas?.outputHeight ?? 0,
      bindingUuid: binding?.uuid ?? null,
      boundButDisabled: !binding && this.#boundProfiles.has(p.uuid),
    };
  }
}

export const destinationIdentityStore = new DestinationIdentityStore();
