import { untrack } from "svelte";
import { obs } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
import type { ChannelStats, ChannelStatEntry, ViewerCounts } from "$lib/api/bridge";
import { oauthStore } from "$lib/stores/oauthStore.svelte";

// Return a copy of `map` with only the keys in `valid`, or null when nothing was
// dropped (so callers skip a no-op reassignment that would re-trigger reactivity).
function prunedToSet<T>(map: Record<string, T>, valid: Set<string>): Record<string, T> | null {
  let dropped = false;
  const out: Record<string, T> = {};
  for (const key of Object.keys(map)) {
    if (valid.has(key)) {
      out[key] = map[key];
    } else {
      dropped = true;
    }
  }
  return dropped ? out : null;
}

// One merged row per connected account, combining the three sources that each
// describe the same channel from a different angle:
//   - identity (name/login/avatar/link-state) from oauthStore (oauth.status)
//   - audience totals (followers/subscribers) from the channels.stats poller
//   - live concurrent viewers from viewers.changed (only pushed while live)
// The UI surfaces (Streams rows, Edit Stream Info, Channels dock) read `rows`
// and never touch the three feeds directly.
export interface ChannelRow {
  accountId: string;
  providerId: string;
  displayName: string;
  login: string;
  avatarUrl: string;
  connected: boolean;
  needsReconnect: boolean;
  audienceCount: number; // -1 unknown/hidden
  audienceKind: "followers" | "subscribers" | "";
  audienceHidden: boolean;
  audienceUpdatedNs: number;
  viewers: number; // -1 when not live/unknown
}

class ChannelsStore {
  // channels.stats sends the full per-account map, but merge (spread) rather than
  // replace so a partial push never drops an account's last-known audience.
  #audience = $state<Record<string, ChannelStatEntry>>({});
  // viewers.changed carries the current live set as a whole; replace outright so an
  // account that dropped out of the map falls back to -1 (not live).
  #viewers = $state<Record<string, number>>({});
  #started = false;
  #off: Array<() => void> = [];

  // Identity drives the row set: one row per oauth.status account, decorated with
  // whatever audience/viewer data has arrived for it.
  readonly rows = $derived.by<ChannelRow[]>(() =>
    oauthStore.statuses.map((s) => {
      const a = this.#audience[s.accountId];
      const v = this.#viewers[s.accountId];
      return {
        accountId: s.accountId,
        providerId: s.providerId,
        displayName: s.displayName,
        login: s.login,
        avatarUrl: s.avatarUrl ?? "",
        connected: s.connected,
        needsReconnect: s.needsReconnect,
        audienceCount: a?.audienceHidden ? -1 : (a?.audienceCount ?? -1),
        audienceKind: a?.audienceKind ?? "",
        audienceHidden: a?.audienceHidden ?? false,
        audienceUpdatedNs: a?.audienceUpdatedNs ?? 0,
        viewers: v ?? -1,
      };
    }),
  );

  /** Wire the merge feeds once at app startup; returns a teardown. Also holds a
   * permanent oauthStore subscription so `statuses` (and thus `rows`) stays
   * populated app-wide, independent of which page happens to be mounted. */
  init(): () => void {
    if (this.#started) {
      return () => {};
    }
    this.#started = true;
    const offOauth = oauthStore.subscribe();
    // Prune audience/viewer entries for accounts that have left the connected set, so
    // the merge maps can't accumulate rows for disconnected accounts. Depends only on
    // `statuses`; the map reads are untracked so a stats/viewers push doesn't re-run
    // this, and the guarded reassignment avoids a self-triggering write.
    const disposePrune = $effect.root(() => {
      $effect(() => {
        const valid = new Set(oauthStore.statuses.map((s) => s.accountId));
        untrack(() => {
          const a = prunedToSet(this.#audience, valid);
          if (a) {
            this.#audience = a;
          }
          const v = prunedToSet(this.#viewers, valid);
          if (v) {
            this.#viewers = v;
          }
        });
      });
    });
    this.#off = [
      offOauth,
      disposePrune,
      obs.on(EV.channelsStats, (p: ChannelStats) => {
        this.#audience = { ...this.#audience, ...p.perAccount };
      }),
      obs.on(EV.viewersChanged, (p: ViewerCounts) => {
        this.#viewers = { ...(p.perAccount ?? {}) };
      }),
    ];
    return () => {
      for (const off of this.#off) {
        off();
      }
      this.#off = [];
      this.#started = false;
    };
  }
}

export const channelsStore = new ChannelsStore();
