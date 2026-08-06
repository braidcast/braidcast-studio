// Shared reactive concurrent-viewer counts: the owner of the `viewers.changed` push
// beyond the per-account slice channelsStore already merges. The Studio bar wants the
// aggregate plus how many destinations answered; the Multistream dock wants one row's
// figure. Both used to subscribe to the event themselves and re-derive their own view
// of the same payload, which is how the two halves of one push came to disagree about
// what "no number" means.
//
// A sibling of multistreamStatusStore rather than a member of it, deliberately. That
// store's whole contract is the ENGINE's per-output state: keyed by bindingUuid,
// fetchable on demand (`multistream.status`), re-polled when a binding toggles. These
// counts are the PLATFORMS' answer: keyed by destination (account x stream profile),
// push-only with no method to read them, and absent for a destination whose provider
// simply cannot report. Merging them would give one store two key spaces, two error
// models and a `loaded` flag that means nothing for half its fields -- and would leave
// "reporting count / armed count" one line away, which is the exact conflation this
// split exists to prevent: the two sides are keyed differently and their ratio is
// meaningless.

import { obs } from "$lib/api/bridge";
import { destinationKey } from "$lib/api/destinationKeys";
import { RefCountedSubscription } from "$lib/stores/refCountedSubscription";
import { EV } from "$lib/utils/eventNames";
import type { ViewerCounts } from "$lib/api/bridge";

class ViewerCountStore {
  // The last push in full, or null when no cycle has reported (before the first push,
  // and again once the stream stops). Null is not zero and must never collapse into it:
  // a destination reporting a real 0 viewers and a destination that never answered are
  // different facts, and every consumer renders them differently.
  #counts = $state<ViewerCounts | null>(null);

  // Destination key -> that destination's reported figure, taken from each row's own
  // `key` so the host's naming is never re-derived here. A destination the host could
  // not read pushes no row at all, so absence from this map IS the absence signal.
  #byKey = $derived.by<Map<string, number>>(() => {
    const m = new Map<string, number>();
    for (const d of this.#counts?.perDestination ?? []) {
      m.set(d.key, d.count);
    }
    return m;
  });

  /** Concurrent viewers summed across every account, or null when no cycle has
   * reported -- render nothing, not a zero. */
  readonly total = $derived(this.#counts?.total ?? null);

  /** providerId -> that platform's viewers, summed over its accounts (two accounts on
   * one platform add into one entry). A platform none of whose accounts reported is
   * ABSENT from the map rather than present at 0, so a per-platform surface cannot
   * claim a live zero for a platform that said nothing. Callers own the ordering and
   * labelling; the store answers with figures, not presentation. */
  readonly perPlatform = $derived.by<Map<string, number>>(() => {
    const m = new Map<string, number>();
    for (const [accountId, n] of Object.entries(this.#counts?.perAccount ?? {})) {
      // accountId is "<providerId>:<userId>" -- the host's OAuth::AccountId.
      const providerId = accountId.split(":")[0];
      m.set(providerId, (m.get(providerId) ?? 0) + n);
    }
    return m;
  });

  /** How many destinations answered THIS cycle: the count REPORTING, never the count
   * armed. The host pushes no row for a destination whose provider is unsupported, not
   * live or errored, so a total that keeps ticking while this drops is how a silently
   * dead destination stops hiding inside the sum.
   *
   * Null when the host sent no breakdown at all, so a consumer prints no denominator
   * rather than a false "/ 0"; 0 when it sent an empty one, which truthfully means
   * nothing answered. Do not pair this with a count of armed destinations -- that side
   * is keyed by bindingUuid, and the ratio of two different key spaces is a lie. */
  readonly reporting = $derived(this.#counts?.perDestination?.length ?? null);

  #feed = new RefCountedSubscription(() => {
    const offViewers = obs.on(EV.viewersChanged, (p) => (this.#counts = p));
    // The poller stops with the stream and never pushes a final zero, so without this
    // an offline stream would keep displaying its last counts indefinitely.
    const offStreaming = obs.on(EV.streamingChanged, (p) => {
      if (!p.active) {
        this.#counts = null;
      }
    });
    return () => {
      offViewers();
      offStreaming();
      // Unsubscribed means unfed: the retained payload can only rot, and the next
      // subscriber must show nothing until a real push arrives.
      this.#counts = null;
    };
  });

  // Ref-counted: first subscriber wires the events, last release tears down. Two
  // consumers mount and unmount independently (the dock also runs alone in a detached
  // window, where the Studio page does not exist), so neither may end the other's feed.
  // Returns an unsubscribe that is safe to call more than once.
  subscribe(): () => void {
    return this.#feed.subscribe();
  }

  /**
   * One destination's reported viewers, or null when it did not report -- off, not yet
   * live, unsupported by its platform, or errored. Only a real row yields a number, so
   * a genuine 0 stays distinguishable from silence.
   *
   * A platform that creates a broadcast per go-live reports per stream profile; one that
   * edits a single persistent channel reports account-wide (empty profile half). The
   * profile-scoped key is therefore tried first and the account-wide key second -- both
   * spelled by destinationKey(), which is the host's own naming.
   */
  countFor(accountId: string, profileUuid?: string | null): number | null {
    if (!accountId) {
      // A key/RTMP/WHIP destination has no account behind it, so nothing polls it.
      return null;
    }
    const scoped = this.#byKey.get(destinationKey(accountId, profileUuid));
    if (scoped !== undefined) {
      return scoped;
    }
    return this.#byKey.get(destinationKey(accountId)) ?? null;
  }
}

export const viewerCountStore = new ViewerCountStore();
