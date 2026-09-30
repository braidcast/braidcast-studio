// Shared host-pushed stats. The host samples once per second and pushes
// `stats.changed` to EVERY browser, so the main window and each detached dock
// window render the same host truth. Each window used to poll `stats.get` on its own
// renderer timer, which meant a detached window's poll could die on its own (a
// throttled or dead renderer) and leave that panel showing a frozen snapshot with no
// cue -- pre-broadcast zeros are indistinguishable from a dead stream. Ref-counted
// the same way multistreamStatusStore is: the first subscriber seeds + wires the
// event, the last unsubscribe tears down and leaves the final snapshot in place.

import { obs, type Stats } from "$lib/api/bridge";
import { RefCountedSubscription } from "$lib/stores/refCountedSubscription";
import { EV } from "$lib/utils/eventNames";

// A sample older than this is no longer a reading. Three host ticks, so an ordinarily
// late tick can't flap the flag.
const STALE_AFTER_MS = 3000;

// The host samples capture rates only while an output is live or a stats.watchCaptures
// lease holds (5 s). Renewing at under half the lease means one late or lost renewal
// cannot let it lapse while a panel is on screen.
const CAPTURE_LEASE_RENEW_MS = 2000;
// The host's lease length until its first answer says otherwise (stats.watchCaptures
// returns it as leaseMs).
const CAPTURE_LEASE_MS_DEFAULT = 5000;

class StatsStore {
  stats = $state<Stats | null>(null);
  error = $state<string | null>(null);
  // Advanced by the watchdog below, not by the samples: staleness is a function of
  // elapsed time, and the case it detects is precisely "no new sample arrived".
  #nowMs = $state(Date.now());

  /** Age of the newest sample in ms; 0 before the first one arrives. */
  ageMs = $derived(this.stats === null ? 0 : Math.max(0, this.#nowMs - this.stats.sampledAtMs));

  /** True when the newest sample is too old to be a live reading -- the push stream
   * or the host sampler stopped. A consumer MUST surface this: the numbers stay on
   * screen either way, and unlabeled frozen values read as live ones. */
  stale = $derived(this.stats !== null && this.ageMs > STALE_AFTER_MS);

  #load(): void {
    obs
      .call("stats.get")
      .then((s) => {
        this.stats = s;
        this.error = null;
        this.#nowMs = Date.now();
      })
      .catch((e) => (this.error = (e as Error).message));
  }

  #feed = new RefCountedSubscription(() => {
    this.#load();
    // A push is an authoritative read: it supersedes a failed seed, and leaving
    // `error` set would keep the panel showing a failure over live numbers.
    const off = obs.on(EV.statsChanged, (s) => {
      this.stats = s;
      this.error = null;
      this.#nowMs = Date.now();
    });
    const timer = setInterval(() => (this.#nowMs = Date.now()), 1000);
    return () => {
      off();
      clearInterval(timer);
    };
  });

  /** Ref-counted subscription; returns an unsubscribe. The first subscriber seeds the
   * snapshot and wires the push, the last one drops both. */
  subscribe(): () => void {
    return this.#feed.subscribe();
  }

  // --- capture-rate lease ------------------------------------------------------
  // Off air the host samples capture rates only for a viewer holding its lease. Every
  // panel showing the rates takes a reference here rather than renewing on its own
  // timer, so two surfaces on screen at once share one renewal, and one closing cannot
  // end the other's lease: there is no release call, the lease simply lapses once the
  // last reference goes. It also renews only while this window is visible.

  /** Wall clock (epoch ms, the clock the host stamps `sampledAtMs` with) by which the
   * current lease had been granted; null while this window holds none. The host grants
   * it and samples on one thread, so any sample taken strictly after this carried it. */
  #leaseGrantedMs = $state<number | null>(null);
  /** Set when a renewal failed; the rows cannot be trusted to appear. */
  captureWatchError = $state<string | null>(null);
  // Bumped on every stop, so a renewal still in flight when the lease was dropped
  // cannot mark a lease this window no longer holds as granted.
  #leaseGen = 0;
  // The renewal still unanswered, or null. A host busy on its UI thread answers nothing
  // for a while; without this, every missed interval queues another call and they all
  // land at once when it frees up. One unanswered for a whole lease is taken as lost
  // (the bridge call has no timeout of its own), so renewal is never held off longer
  // than the lease it is keeping alive.
  #pendingRenewal: { gen: number; sentMs: number } | null = null;
  #leaseMs = CAPTURE_LEASE_MS_DEFAULT;
  #leaseTimer: ReturnType<typeof setInterval> | null = null;

  /** Whether `stats.captures` is the whole truth rather than "not sampled yet": it has
   * rows, or it comes from a sample the lease covered. False from the moment a panel
   * appears until the host's next tick, which is when an empty list starts meaning
   * "nothing to list". */
  capturesSettled = $derived(
    this.stats !== null &&
      (this.stats.captures.length > 0 ||
        (this.#leaseGrantedMs !== null && this.stats.sampledAtMs > this.#leaseGrantedMs)),
  );

  #renewLease = (): void => {
    const gen = this.#leaseGen;
    const now = Date.now();
    const pending = this.#pendingRenewal;
    if (pending && pending.gen === gen && now - pending.sentMs < this.#leaseMs) {
      return;
    }
    if (pending && pending.gen === gen) {
      // Lost: the host lease lapsed while it was unanswered, so only a sample after the
      // next grant may settle an empty list.
      this.#leaseGrantedMs = null;
    }
    const renewal = { gen, sentMs: now };
    this.#pendingRenewal = renewal;
    obs
      .call("stats.watchCaptures")
      .then((r) => {
        if (gen !== this.#leaseGen) {
          return;
        }
        if (r && r.leaseMs > 0) {
          this.#leaseMs = r.leaseMs;
        }
        this.captureWatchError = null;
        this.#leaseGrantedMs ??= Date.now();
      })
      .catch((e) => {
        if (gen === this.#leaseGen) {
          this.captureWatchError = (e as Error).message;
          // The host lease may lapse before the next success, so that success is a
          // fresh grant: only a sample after it may settle an empty list.
          this.#leaseGrantedMs = null;
        }
      })
      .finally(() => {
        if (this.#pendingRenewal === renewal) {
          this.#pendingRenewal = null;
        }
      });
  };

  #stopLease(): void {
    this.#leaseGen++;
    if (this.#leaseTimer !== null) {
      clearInterval(this.#leaseTimer);
      this.#leaseTimer = null;
    }
    this.#leaseGrantedMs = null;
    this.captureWatchError = null;
  }

  // Renews immediately on becoming visible, since a lease left to lapse while hidden
  // is gone, and stops outright while hidden.
  #applyLeaseVisibility = (): void => {
    if (document.visibilityState === "visible") {
      if (this.#leaseTimer === null) {
        this.#renewLease();
        this.#leaseTimer = setInterval(this.#renewLease, CAPTURE_LEASE_RENEW_MS);
      }
    } else {
      this.#stopLease();
    }
  };

  #captureLease = new RefCountedSubscription(() => {
    this.#applyLeaseVisibility();
    document.addEventListener("visibilitychange", this.#applyLeaseVisibility);
    return () => {
      document.removeEventListener("visibilitychange", this.#applyLeaseVisibility);
      this.#stopLease();
    };
  });

  /** Keep the host sampling capture rates while the caller shows them. Ref-counted;
   * returns a release that is safe to call twice. Take it only while the rates are
   * actually on screen (see `whileVisible`), and alongside `subscribe`, which is what
   * delivers the samples. */
  watchCaptures(): () => void {
    return this.#captureLease.subscribe();
  }
}

export const statsStore = new StatsStore();
