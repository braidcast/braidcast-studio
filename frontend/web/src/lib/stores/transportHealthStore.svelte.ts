// Shared reactive transport health (R14/G1): the connection state of every chat,
// events, and overlay transport, surfaced so a dead transport no longer shows a
// stale green badge. The sibling of multistreamStatusStore -- same shape, same
// lifecycle: the rows come from `transports.health`, are pushed on
// `transports.healthChanged`, and this singleton is the one source of truth so no
// consumer hand-rolls the fetch+subscribe. Data only; the visual badge is a design
// decision left to the consumer.

import { obs } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
import type { TransportHealth, TransportHealthState } from "$lib/api/bridge";

// A state whose transport is up or working toward it -- the "not a problem" set.
// ONE predicate; consumers must not re-derive it per call site.
export function isHealthyState(state: TransportHealthState): boolean {
  return state === "connected" || state === "connecting" || state === "reconnecting";
}

class TransportHealthStore {
  transports = $state<TransportHealth[]>([]);
  loaded = $state(false);
  error = $state<string | null>(null);

  #subs = 0;
  #off: (() => void) | null = null;
  // Per-refresh token: a burst of change events launches concurrent refreshes; drop
  // any resolution that isn't the latest issued so a slow earlier call can't win.
  #seq = 0;

  // transportId -> its health row.
  byId = $derived.by<Map<string, TransportHealth>>(() => {
    const m = new Map<string, TransportHealth>();
    for (const t of this.transports) {
      m.set(t.id, t);
    }
    return m;
  });

  // Ref-counted: first subscriber fetches + wires the event, last unsubscribe tears
  // down. Returns an unsubscribe. Mirrors multistreamStatusStore's lifecycle.
  subscribe(): () => void {
    this.#subs++;
    if (this.#subs === 1) {
      void this.refresh();
      const off = obs.on(EV.transportsHealthChanged, (p) => (this.transports = p.transports));
      this.#off = () => off();
    }
    return () => {
      this.#subs--;
      if (this.#subs === 0) {
        this.#off?.();
        this.#off = null;
      }
    };
  }

  async refresh(): Promise<void> {
    const seq = ++this.#seq;
    try {
      const status = await obs.call("transports.health");
      if (seq !== this.#seq) {
        return;
      }
      this.transports = status.transports;
      this.error = null;
    } catch (e) {
      if (seq !== this.#seq) {
        return;
      }
      this.error = (e as Error).message;
    } finally {
      this.loaded = true;
    }
  }

  // The state of one transport by id, or "disconnected" when it has never reported
  // (no row yet). ONE definition; consumers must not default the absent case per site.
  stateOf(id: string): TransportHealthState {
    return this.byId.get(id)?.state ?? "disconnected";
  }
}

export const transportHealthStore = new TransportHealthStore();
