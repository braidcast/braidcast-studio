// Shared reactive multistream live-status. The third leg of the canvas data model
// (alongside canvasStore and outputBindingStore): the live per-output state rows
// from `multistream.status`, pushed on `multistream.changed`, and re-polled on
// `outputBinding.changed` (a binding toggle changes which rows are live). Every
// consumer -- the Studio bar, Multistream dock, Canvases page, per-canvas CanvasDock,
// and now the Go Live modal -- used to hand-roll this fetch+subscribe into private state and then
// re-derive the per-canvas "strongest state" reduction independently (which had
// drifted: some ordered error before connecting, some the reverse). This singleton
// is the one source of truth, and deriveCanvasState() is the one reduction.

import { obs } from "$lib/api/bridge";
import { RefCountedSubscription } from "$lib/stores/refCountedSubscription";
import { EV } from "$lib/utils/eventNames";
import { titleState } from "$lib/utils/format";
import { DETACHED_DOCK } from "$lib/utils/windowContext";
import type { MultistreamStatus, MultistreamState, OutputBindingInfo } from "$lib/api/bridge";

// The strongest live state across a set of outputs: live wins, then reconnecting
// (a dropped stream the engine is still retrying), then connecting, then error,
// else idle. ONE ordering, shared by every consumer -- see the file header for
// why this can't live per call site.
export function reduceStates(states: MultistreamState[]): MultistreamState {
  if (states.includes("live")) {
    return "live";
  }
  if (states.includes("reconnecting")) {
    return "reconnecting";
  }
  if (states.includes("connecting")) {
    return "connecting";
  }
  if (states.includes("error")) {
    return "error";
  }
  return "idle";
}

// A state whose output is running (connected, or still dialing/retrying) --
// mirrors the native IsCanvasLive gate, which holds the canvas through a
// reconnect. ONE predicate; consumers must not re-derive it per call site.
export function isActiveState(state: MultistreamState | "off" | "disabled"): boolean {
  return state === "live" || state === "connecting" || state === "reconnecting";
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

// Hover detail for a row's state tag: the drop reason while the engine retries
// (the tag alone says "reconnecting" but not why). Empty when there is nothing
// to surface. Reached through bindingRowStatus below, which is what the two
// destination panels render from.
function bindingRowDetail(
  b: OutputBindingInfo,
  statusByBinding: Map<string, MultistreamStatus>,
): string {
  const row = statusByBinding.get(b.uuid);
  if (row?.state !== "reconnecting" || !row.lastError) {
    return "";
  }
  return `Reconnecting: ${row.lastError}`;
}

// Is anything on the air right now? The predicate itself, for consumers holding their
// own row map; multistreamStatusStore.anyLive below is the same answer off the store.
//
// The TEST matches the host's -- MultistreamEngine::AnyLive() scans for IsActiveState,
// which isActiveState above mirrors member for member. The ROW SETS do not match, and
// the difference is one-directional: Statuses() skips every disabled binding, while
// AnyLive() scans the whole live vector, so an output still running under a binding
// disabled mid-broadcast counts as live for the host and enumerates nowhere for us
// (bridge.cpp says so in its own words: "live to nothing we can name" must not read as
// "not live").
//
// So this is a SUBSET of the host's answer: it can read false while the host reads true,
// never the reverse. That direction is the safe one -- the UI falls back to treating the
// moment as pre-live, which is what it did before any of this existed, and it can never
// offer an arm the host would then refuse. Do not reason from equality here.
export function anyOutputLive(statusByBinding: Map<string, MultistreamStatus>): boolean {
  for (const o of statusByBinding.values()) {
    if (isActiveState(o.state)) {
      return true;
    }
  }
  return false;
}

/** How one destination row reads: the color key it borrows from STATE_COLOR_EXT, the
 * word printed beside it, its two grades of explanation, and whether it can be brought
 * back onto the running broadcast. */
export interface BindingRowStatus {
  state: MultistreamState | "disabled";
  label: string;
  /** Hover text for the tag, in every state that carries one -- including the
   * reconnecting reason, which has always lived here. */
  detail: string;
  /** The sentence a panel is allowed to render as visible body text. Set ONLY for the
   * two mid-stream states this function introduces, so no row that already had a
   * hover-only detail grows a line it never had. */
  note: string;
  retryable: boolean;
}

// The sentence a staged row carries. It names the ONE remedy, so the remedy has to be
// reachable from the window reading it: a detached dock mounts a single dock and never
// the app shell, so Stream Information cannot open there and destinationArming refuses
// the mid-stream arm outright. Pointing that window at a dialog it cannot show would be
// the same wrong answer the refusal exists to avoid.
const STAGED_REMEDY =
  DETACHED_DOCK === null
    ? "Add it to this one from Stream Information."
    : "Add it to this one from the main window.";
const STAGED_DETAIL = `Not on this stream — armed for your next go-live. ${STAGED_REMEDY}`;
// Stands in whenever a dropped row has no reason to quote, which is TWO different
// endings, not one: a clean stop (the platform ended the broadcast, so the output closed
// with OBS_OUTPUT_SUCCESS and there was never an error) and a genuine failure libobs
// reported no `last_error` for (OnOutputStop stores "" then). Worded to claim neither --
// "left the broadcast" would read as orderly over a failure, and the row's red already
// says something is wrong.
const DROPPED_DETAIL = "This destination is no longer on the broadcast.";

// A row's whole presentation, in one place, because the Canvases destinations tab and
// the Multistream dock render the same row and each used to map state -> word itself.
//
// `anyLive` is what splits idle in two. With nothing on the air, idle means "waiting
// for the next go-live" and reads exactly as it always has. With a broadcast running,
// the same idle covers two states the engine's report flattens together: a binding that
// was never on this stream, and one that WAS and is not now -- which is what a platform
// ending a broadcast produces, since that connection closes with a success code.
// startedThisSession is the only thing that tells them apart, and only the second is
// retryable: a destination that never went out owns no broadcast to restart, so it has
// to go through the stream-info step instead.
export function bindingRowStatus(
  b: OutputBindingInfo,
  statusByBinding: Map<string, MultistreamStatus>,
  anyLive: boolean,
): BindingRowStatus {
  const state = bindingRowState(b, statusByBinding);
  // Nothing on the air takes every row back to what it has always read as, deliberately
  // including a destination that dropped: with no broadcast left, recovery is an
  // ordinary go-live rather than a mid-stream rejoin, and the host refuses ArmBindingLive
  // outright when nothing is live -- so a Retry offered here would be an affordance that
  // cannot work. Do not "fix" this into one. startedThisSession still earns its keep in
  // the multi-destination case, where one destination drops off a broadcast that carries
  // on without it, which is the case this whole split exists for.
  if (state === "disabled" || isActiveState(state) || !anyLive) {
    return {
      state,
      label: titleState(state),
      detail: bindingRowDetail(b, statusByBinding),
      note: "",
      retryable: false,
    };
  }
  const row = statusByBinding.get(b.uuid);
  // An error row is dropped whatever the session flag reports: the engine only records
  // one against an output it actually ran.
  if (state === "error" || row?.startedThisSession) {
    const reason = row?.lastError || DROPPED_DETAIL;
    return { state: "error", label: "Dropped", detail: reason, note: reason, retryable: true };
  }
  return { state: "idle", label: "Standby", detail: STAGED_DETAIL, note: STAGED_DETAIL, retryable: false };
}

class MultistreamStatusStore {
  outputs = $state<MultistreamStatus[]>([]);
  loaded = $state(false);
  error = $state<string | null>(null);

  // Per-refresh token: a burst of change events launches concurrent refreshes; drop
  // any resolution that isn't the latest issued so a slow earlier call can't win.
  #seq = 0;

  // bindingUuid -> its live status row (only enabled bindings appear in `outputs`).
  statusByBinding = $derived.by<Map<string, MultistreamStatus>>(() => {
    const m = new Map<string, MultistreamStatus>();
    for (const o of this.outputs) {
      m.set(o.bindingUuid, o);
    }
    return m;
  });

  // Whether any output is on the air, memoized off the rows. A SUBSET of the host's
  // AnyLive() -- same test, narrower row set, see anyOutputLive above -- which is why it
  // can never offer an arm the host would refuse.
  //
  // THE liveness read for anything deciding "is this a mid-stream action": the arm
  // toggle, the modal's arm switch, the count on its primary, and the revert on its
  // close all take it from here. The global streaming flag answers the same question
  // (bridge.cpp reports `active` AS AnyLive()), but it arrives on a different event
  // stream, and two doors seeding off two differently-fresh answers is how the panel
  // switch and the dialog switch came to disagree about one destination.
  //
  // Reads false before the first poll resolves, which is a real answer only once
  // `loaded` is true -- callers that can run that early refresh first.
  anyLive = $derived(anyOutputLive(this.statusByBinding));

  #feed = new RefCountedSubscription(() => {
    void this.refresh();
    // An authoritative push is a successful read: it supersedes a failed poll, and
    // leaving `error` set would pin consumers that refuse while it is non-null (the
    // Studio go-live bar) in a permanently blocked state.
    const offMulti = obs.on(EV.multistreamChanged, (p) => {
      this.outputs = p.outputs;
      this.error = null;
      this.loaded = true;
    });
    // A binding enable/disable changes which outputs are live but doesn't push a
    // multistream.changed, so re-poll on it too.
    const offBindings = obs.on(EV.outputBindingChanged, () => void this.refresh());
    return () => {
      offMulti();
      offBindings();
    };
  });

  // Ref-counted: first subscriber fetches + wires events, last unsubscribe tears
  // down. Returns an unsubscribe. Mirrors statsStore/oauthStore lifecycle.
  subscribe(): () => void {
    return this.#feed.subscribe();
  }

  async refresh(): Promise<void> {
    const seq = ++this.#seq;
    try {
      const status = await obs.call("multistream.status");
      if (seq !== this.#seq) {
        return;
      }
      this.outputs = status.outputs;
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
