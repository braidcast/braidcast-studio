import { obs, type OAuthStatus, type OAuthProvider, type ChatPlatform } from "./bridge";
import { EV } from "./eventNames";
import { PLATFORM_ORDER } from "./theme/platformColors";

// Shared reactive OAuth-account store. `oauth.status` has a push event (fired on any
// link-state change, carrying the full row set), but consumers used to refetch it
// independently (StreamsTab, GoLiveModal). This ref-counts subscribers and wires the
// bridge subscription ONCE while >=1 consumer is active: the first subscriber does the
// initial fetch (status + providers) and opens the event stream; the last unsubscribe
// closes it, leaving the final snapshot in place. Mirrors statsStore's ref-count.
//
// `providers` is fetched once for completeness (a build without a platform client id
// returns an empty list); the connected-platform ordering itself comes from the fixed
// PLATFORM_ORDER so it never reshuffles.
class OAuthStore {
  statuses = $state<OAuthStatus[]>([]);
  providers = $state<OAuthProvider[]>([]);
  #subs = 0;
  #off: (() => void) | undefined;

  /** Re-fetch status + providers now. Callers awaiting a post-mutation refresh (a
   * connect/disconnect that also emits oauth.status, refreshing subscribers) use this
   * to sequence their own UI. */
  async refresh(): Promise<void> {
    await Promise.allSettled([
      obs.call("oauth.status").then((s) => (this.statuses = s)),
      obs.call("oauth.providers").then((p) => (this.providers = p)),
    ]);
  }

  /** Resolves after a FRESH status+providers load settles, for one-shot consumers
   * (the Go Live modal's prefill gate). Does NOT open a subscription — it awaits a
   * fresh one-shot fetch each call, so the awaiter never resolves against a stale
   * snapshot while a refetch is still in flight. A live subscription (if any) keeps
   * `statuses` current afterward. */
  async whenReady(): Promise<void> {
    await this.refresh();
  }

  /** Accounts with a live token (green "linked" state). Excludes needs-reconnect rows,
   * which report connected:false. */
  get connectedAccounts(): OAuthStatus[] {
    return this.statuses.filter((s) => s.connected);
  }

  /** Distinct providerIds that have >=1 connected account, in PLATFORM_ORDER order. */
  get connectedPlatforms(): ChatPlatform[] {
    const have = new Set(this.connectedAccounts.map((s) => s.providerId));
    return PLATFORM_ORDER.filter((p) => have.has(p));
  }

  get hasAnyConnected(): boolean {
    return this.connectedAccounts.length > 0;
  }

  /** Ref-counted subscription; returns an unsubscribe. Fetch + subscribe on the first
   * subscriber, tear down on the last. */
  subscribe(): () => void {
    this.#subs++;
    if (this.#subs === 1) {
      void this.refresh();
      this.#off = obs.on(EV.oauthStatus, (s) => (this.statuses = s));
    }
    return () => {
      this.#subs--;
      if (this.#subs === 0) {
        this.#off?.();
        this.#off = undefined;
      }
    };
  }
}

export const oauthStore = new OAuthStore();
