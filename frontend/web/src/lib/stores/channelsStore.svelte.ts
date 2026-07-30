import { untrack } from "svelte";
import { obs } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
import type {
  AudienceKind,
  ChannelStats,
  ChannelStatDestination,
  ChannelStatEntry,
  ViewerCounts,
} from "$lib/api/bridge";
import { oauthStore } from "$lib/stores/oauthStore.svelte";

// Return a copy of `map` holding only the entries `keep` accepts, or null when nothing was
// dropped (so callers skip a no-op reassignment that would re-trigger reactivity). The
// predicate takes the value as well as the key because a destination-keyed map is pruned by
// the account its entry names, not by its own key.
function pruned<T>(map: Record<string, T>, keep: (key: string, value: T) => boolean): Record<string, T> | null {
  let dropped = false;
  const out: Record<string, T> = {};
  for (const key of Object.keys(map)) {
    if (keep(key, map[key])) {
      out[key] = map[key];
    } else {
      dropped = true;
    }
  }
  return dropped ? out : null;
}

// One merged row per connected DESTINATION, combining the three sources that each
// describe the same channel from a different angle:
//   - identity (name/login/avatar/link-state) from oauthStore (oauth.status)
//   - audience totals (followers/subscribers) from the channels.stats poller
//   - live concurrent viewers from viewers.changed (only pushed while live)
// The UI surfaces (Streams rows, Edit Stream Info, Channels dock) read `rows`
// and never touch the three feeds directly.
//
// An account whose reads name no stream profile yields exactly one row keyed by its
// accountId -- which is every platform whose account IS its single destination, so their
// rows are identical to what they were before destinations existed here.
export interface ChannelRow {
  /** The host's destination key: "accountId@profileUuid", or the bare accountId for an
   * account-wide row. The row's identity -- an account can produce several. */
  key: string;
  accountId: string;
  /** Which stream profile this row is the destination of; "" for an account-wide row. */
  profileUuid: string;
  providerId: string;
  displayName: string;
  login: string;
  avatarUrl: string;
  connected: boolean;
  needsReconnect: boolean;
  audienceCount: number; // -1 unknown/hidden
  audienceKind: AudienceKind;
  audienceHidden: boolean;
  audienceUpdatedNs: number;
  viewers: number; // -1 when not live/unknown
}

type OAuthStatusRow = (typeof oauthStore.statuses)[number];

class ChannelsStore {
  // channels.stats sends the full per-account map, but merge (spread) rather than
  // replace so a partial push never drops an account's last-known audience.
  #audience = $state<Record<string, ChannelStatEntry>>({});
  // The same for the per-destination rows the same payload carries, and for the same
  // reason: Kick's live follower push names one account and no destination at all.
  #audienceByDestination = $state<Record<string, ChannelStatDestination>>({});
  // viewers.changed carries the current live set as a whole; replace outright so an
  // account that dropped out of the map falls back to -1 (not live).
  #viewers = $state<Record<string, number>>({});
  #viewersByDestination = $state<Record<string, number>>({});
  #started = false;
  #off: Array<() => void> = [];

  // accountId -> its per-destination audience rows, key-ordered so a card cannot change
  // place between pushes. Account-wide rows are excluded: that row IS the account, and
  // admitting it here would give a single-destination account two cards.
  #destinationsByAccount = $derived.by<Record<string, ChannelStatDestination[]>>(() => {
    const out: Record<string, ChannelStatDestination[]> = {};
    for (const key of Object.keys(this.#audienceByDestination).sort()) {
      const d = this.#audienceByDestination[key];
      if (!d.profileUuid) {
        continue;
      }
      (out[d.accountId] ??= []).push(d);
    }
    return out;
  });

  // Identity drives the row set: one row per oauth.status account, decorated with
  // whatever audience/viewer data has arrived for it -- fanned out to one row per
  // destination for an account that reports several.
  readonly rows = $derived.by<ChannelRow[]>(() =>
    oauthStore.statuses.flatMap((s) => {
      const dests = this.#destinationsByAccount[s.accountId];
      if (!dests) {
        return [this.#row(s, s.accountId, "", this.#audience[s.accountId])];
      }
      return dests.map((d) => this.#row(s, d.key, d.profileUuid, d));
    }),
  );

  #row(s: OAuthStatusRow, key: string, profileUuid: string, a: ChannelStatEntry | undefined): ChannelRow {
    // A destination row takes only its own viewer figure: falling back to the account's
    // would print the account's whole audience on each of its destinations. An account-wide
    // row keeps the per-account fallback, which is where every single-channel platform's
    // figure arrives.
    const own = this.#viewersByDestination[key];
    const v = profileUuid ? own : (own ?? this.#viewers[s.accountId]);
    return {
      key,
      accountId: s.accountId,
      profileUuid,
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
  }

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
          const a = pruned(this.#audience, (accountId) => valid.has(accountId));
          if (a) {
            this.#audience = a;
          }
          const v = pruned(this.#viewers, (accountId) => valid.has(accountId));
          if (v) {
            this.#viewers = v;
          }
          // Keyed by destination, so the account each entry names decides -- pruning by
          // the key itself would keep every destination of a departed account forever.
          const ad = pruned(this.#audienceByDestination, (_key, d) => valid.has(d.accountId));
          if (ad) {
            this.#audienceByDestination = ad;
          }
        });
      });
    });
    this.#off = [
      offOauth,
      disposePrune,
      obs.on(EV.channelsStats, (p: ChannelStats) => {
        this.#audience = { ...this.#audience, ...p.perAccount };
        const rows: Record<string, ChannelStatDestination> = {};
        for (const d of p.perDestination ?? []) {
          rows[d.key] = d;
        }
        this.#audienceByDestination = { ...this.#audienceByDestination, ...rows };
      }),
      obs.on(EV.viewersChanged, (p: ViewerCounts) => {
        this.#viewers = { ...(p.perAccount ?? {}) };
        const rows: Record<string, number> = {};
        for (const d of p.perDestination ?? []) {
          rows[d.key] = d.count;
        }
        this.#viewersByDestination = rows;
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
