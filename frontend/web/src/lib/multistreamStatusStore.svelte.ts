// Shared reactive multistream live-status. The third leg of the canvas data model
// (alongside canvasStore and outputBindingStore): the live per-output state rows
// from `multistream.status`, pushed on `multistream.changed`, and re-polled on
// `outputBinding.changed` (a binding toggle changes which rows are live). Every
// consumer -- the Studio bar, Multistream dock, Canvases page, per-canvas
// CanvasDock -- used to hand-roll this fetch+subscribe into private state and then
// re-derive the per-canvas "strongest state" reduction independently (which had
// drifted: some ordered error before connecting, some the reverse). This singleton
// is the one source of truth, and deriveCanvasState() is the one reduction.

import { obs } from "./bridge";
import { EV } from "./eventNames";
import type { MultistreamStatus, MultistreamState, OutputBindingInfo } from "./bridge";

// The strongest live state across a set of outputs: live wins, then connecting,
// then error, else idle. ONE ordering, shared by every consumer -- see the file
// header for why this can't live per call site.
export function reduceStates(states: MultistreamState[]): MultistreamState {
  if (states.includes("live")) {
    return "live";
  }
  if (states.includes("connecting")) {
    return "connecting";
  }
  if (states.includes("error")) {
    return "error";
  }
  return "idle";
}

// The effective state of a single destination row: a disabled binding never goes
// live; otherwise its live row's state (idle before the row exists). Takes the
// status map so prop-driven consumers keep their own reference. ONE definition,
// shared by the Multistream dock and the Canvases destinations tab.
export function bindingRowState(
  b: OutputBindingInfo,
  statusByBinding: Map<string, MultistreamStatus>,
): MultistreamState | "disabled" {
  if (!b.enabled) {
    return "disabled";
  }
  return statusByBinding.get(b.uuid)?.state ?? "idle";
}

class MultistreamStatusStore {
  outputs = $state<MultistreamStatus[]>([]);
  loaded = $state(false);
  error = $state<string | null>(null);

  #subs = 0;
  #off: (() => void) | null = null;

  // bindingUuid -> its live status row (only enabled bindings appear in `outputs`).
  statusByBinding = $derived.by<Map<string, MultistreamStatus>>(() => {
    const m = new Map<string, MultistreamStatus>();
    for (const o of this.outputs) {
      m.set(o.bindingUuid, o);
    }
    return m;
  });

  // Ref-counted: first subscriber fetches + wires events, last unsubscribe tears
  // down. Returns an unsubscribe. Mirrors statsStore/oauthStore lifecycle.
  subscribe(): () => void {
    this.#subs++;
    if (this.#subs === 1) {
      void this.refresh();
      const offMulti = obs.on(EV.multistreamChanged, (p) => (this.outputs = p.outputs));
      // A binding enable/disable changes which outputs are live but doesn't push a
      // multistream.changed, so re-poll on it too.
      const offBindings = obs.on(EV.outputBindingChanged, () => void this.refresh());
      this.#off = () => {
        offMulti();
        offBindings();
      };
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
    try {
      const status = await obs.call("multistream.status");
      this.outputs = status.outputs;
      this.error = null;
    } catch (e) {
      this.error = (e as Error).message;
    } finally {
      this.loaded = true;
    }
  }

  // Outputs bound to one canvas (in `outputs` order).
  forCanvas(canvasUuid: string): MultistreamStatus[] {
    return this.outputs.filter((o) => o.canvasUuid === canvasUuid);
  }

  // Strongest live state across a canvas's ENABLED bindings, "off" when none are
  // enabled. Takes the canvas's bindings (from outputBindingStore) so it reflects
  // config even for bindings with no live row yet. THE per-canvas dot reduction.
  deriveCanvasState(bindings: OutputBindingInfo[]): MultistreamState | "off" {
    const enabled = bindings.filter((b) => b.enabled);
    if (enabled.length === 0) {
      return "off";
    }
    return reduceStates(enabled.map((b) => this.statusByBinding.get(b.uuid)?.state ?? "idle"));
  }

  // Strongest state across a raw list of output rows (for consumers that already
  // hold `outputs` filtered by canvas rather than bindings -- e.g. the Studio bar's
  // focused-canvas dot). "off" is not meaningful here; callers map idle as needed.
  deriveOutputsState(rows: MultistreamStatus[]): MultistreamState {
    return reduceStates(rows.map((o) => o.state));
  }
}

export const multistreamStatusStore = new MultistreamStatusStore();
