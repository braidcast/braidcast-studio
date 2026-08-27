// Shared Config Advisor state: runs the read-only sweep and holds the findings.
//
// Cadence is the whole design here. The sweep is one bridge call per scene plus one
// `properties.get` per source of a declared type, so it must NOT ride the 1 Hz stats
// tick. It runs on the first subscriber, on the events that invalidate its result,
// and when the user asks for a rescan. Ref-counted like statsStore /
// multistreamStatusStore: the first subscriber wires the events, the last drops them.

import { obs } from "$lib/api/bridge";
import { buildSnapshot } from "$lib/monitor/advisorScan";
import { evaluate, SETTINGS_TYPE_IDS, type AdvisorRow, type AdvisorSkip } from "$lib/monitor/advisorRules";
import { RefCountedSubscription } from "$lib/stores/refCountedSubscription";
import { EV, type BridgeEvent } from "$lib/utils/eventNames";

// Findings are keyed by source NAME (properties.get resolves sources by name), so a
// rename between detect and display leaves a row addressing something that no longer
// answers to it.
//
// KNOWN EXCEPTION: `sources.renameByName` (frontend/src/bridge.cpp) — the audio-mixer
// rename — announces `sceneItems.changed` only for canvases whose CURRENT scene lists
// the renamed source. A source held solely by a non-current scene, or a global audio
// device (no scene item at all), gets `audio.changed` alone, so renaming one there
// leaves its findings pointing at the old name until the next scene-graph event or a
// Rescan. `audio.changed` is deliberately NOT
// subscribed: OnAudioSourceSetChanged (frontend/src/obs_bootstrap.cpp) emits it from
// the source_activate/source_deactivate signals, so every scene switch would fire a
// second full sweep on top of the one scenes.changed already causes — plus one per
// audio.setAdvanced (mixer-track and per-source advanced-audio edits). That is a
// broad subscription bought for a rare rename path.
const INVALIDATING: readonly BridgeEvent[] = [EV.scenesChanged, EV.sceneItemsChanged, EV.canvasChanged];

// A scene edit fires several of the above in a burst; coalesce them into one sweep.
const RESCAN_DEBOUNCE_MS = 400;

class AdvisorStore {
  rows = $state<AdvisorRow[]>([]);
  /** Rules that could not run against the last snapshot. Non-empty means the result
   * is a partial answer, and the panel must not present it as an all-clear. */
  skipped = $state<AdvisorSkip[]>([]);
  /** True once a sweep has produced a result for the loaded collection. */
  scanned = $state(false);
  scanning = $state(false);
  error = $state<string | null>(null);

  // Per-sweep token: a burst can launch concurrent sweeps; drop any resolution that
  // is not the latest issued so a slow earlier one cannot overwrite a newer one.
  #seq = 0;
  #timer: ReturnType<typeof setTimeout> | null = null;

  #feed = new RefCountedSubscription(() => {
    this.#schedule(0);
    const offs = INVALIDATING.map((e) => obs.on(e, () => this.#schedule(RESCAN_DEBOUNCE_MS)));
    // Only the loaded collection exists in libobs, so findings from the previous one
    // are not merely stale, they address sources that are gone. Clear before rescanning.
    offs.push(
      obs.on(EV.collectionsChanged, () => {
        this.#clear();
        this.#schedule(RESCAN_DEBOUNCE_MS);
      }),
    );
    return () => {
      for (const off of offs) {
        off();
      }
      this.#cancel();
    };
  });

  /** Ref-counted subscription; returns an unsubscribe. */
  subscribe(): () => void {
    return this.#feed.subscribe();
  }

  /** User-initiated resweep. The session log has no change event, so a page whose
   * scene graph is not moving would otherwise never pick up new browser errors. */
  rescan(): void {
    this.#schedule(0);
  }

  #clear(): void {
    // Invalidate any sweep already in flight along with the rows it would land.
    this.#seq++;
    this.rows = [];
    this.skipped = [];
    this.scanned = false;
    this.error = null;
    // The bumped token just orphaned any in-flight sweep, and an orphan returns
    // early without clearing this — so clear it here or the Rescan button stays
    // disabled until the replacement sweep happens to start.
    this.scanning = false;
  }

  #cancel(): void {
    if (this.#timer !== null) {
      clearTimeout(this.#timer);
      this.#timer = null;
    }
  }

  #schedule(delayMs: number): void {
    this.#cancel();
    this.#timer = setTimeout(() => {
      this.#timer = null;
      void this.#scan();
    }, delayMs);
  }

  async #scan(): Promise<void> {
    const seq = ++this.#seq;
    this.scanning = true;
    try {
      const snapshot = await buildSnapshot(SETTINGS_TYPE_IDS);
      if (seq !== this.#seq) {
        return;
      }
      const result = evaluate(snapshot);
      this.rows = result.rows;
      this.skipped = result.skipped;
      this.error = null;
      this.scanned = true;
    } catch (e) {
      if (seq !== this.#seq) {
        return;
      }
      this.error = (e as Error).message;
      this.scanned = true;
    } finally {
      if (seq === this.#seq) {
        this.scanning = false;
      }
    }
  }
}

export const advisorStore = new AdvisorStore();
