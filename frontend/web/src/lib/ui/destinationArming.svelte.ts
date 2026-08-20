// Bringing a destination onto a broadcast that is ALREADY running.
//
// Shared because three surfaces drive the same two actions: the Canvases destinations
// tab and the Multistream dock both own the arm toggle and the row's Retry, and the
// Stream Information modal drives the same prepared start from its primary. A second
// copy of the start call is how one door would come to skip the preparation.
//
// Arming mid-stream is deliberately NOT a start. An account-backed destination owns no
// broadcast until one is prepared for it, and its stream profile still holds the ingest
// address and key the LAST go-live wrote there. Starting an encoder against that pushes
// at a stale endpoint: green on every local indicator, absent from the platform. So the
// toggle persists the flag and hands the user to the modal, which is where the stream
// info is validated and the prepared start is driven from.

import { obs } from "$lib/api/bridge";
import { openGoLiveModal } from "$lib/dialogs/golive/goLiveModalOpener.svelte";
import { isActiveState, multistreamStatusStore } from "$lib/stores/multistreamStatusStore.svelte";
import { outputBindingStore } from "$lib/stores/outputBindingStore.svelte";
import { showToast } from "$lib/stores/toastStore.svelte";
import { EV } from "$lib/utils/eventNames";
import { joinNames } from "$lib/utils/format";
import { DETACHED_DOCK } from "$lib/utils/windowContext";

/** One report that some destinations could not be brought up. */
export interface DestinationFailure {
  /** Sentence opener, ending in a space: "Couldn't start ", "Not going live — couldn't prepare ". */
  lead: string;
  /** Who it is about. One reads by name, several collapse to a count. */
  names: string[];
  /** Why, for the SINGLE-destination case. Rendered as the toast's one visible line. May
   * be empty -- the host sends "" when it has no reason to give and an Error can carry a
   * blank message -- and an empty one renders no line rather than an empty one. */
  reason: string;
  /** One visible line per failure. Used in the PLURAL case only, where a single shared
   * `reason` would attribute one destination's cause to all of them. */
  lines?: string[];
  /** Appended after the name in the SINGULAR branch only, where the lead alone would be
   * ambiguous about what failed ("Couldn't update Twitch stream info"). */
  singularSuffix?: string;
  assertive?: boolean;
}

// How long a toast carrying reason lines stays up. The 4s default sizes a headline; a lead
// plus one host diagnostic per destination is body text, and on the hotkey, tray and
// scheduler paths this toast is the only place that text ever appears. Sized for the common
// case of one or two short reasons -- the content has no ceiling, since a wrapped Err chain
// for several destinations can outrun any figure. Set here rather than at the call sites,
// so no failure surface reads faster than another.
const READ_MS = 9000;

/**
 * THE wording for a destination-level failure, because the same sentence was authored at
 * five sites -- the modal's metadata push and its mid-stream starts, this module's Retry,
 * and the Studio page's arm-failed and start-failed handlers -- and had already drifted:
 * two of them never aggregated at all, and the same event was assertive in one and polite
 * in another. Naming one destination and counting several is the shape; the caller owns
 * only the lead, because the lead is the one part that genuinely differs.
 */
export function destinationFailureToast(f: DestinationFailure): void {
  const one = f.names.length === 1;
  const message = one
    ? f.lead + f.names[0] + (f.singularSuffix ?? "")
    : f.lead + f.names.length + " destinations";
  // The cause is visible, wrapping text in both branches. Singular carries the reason bare
  // -- the message has already named the destination, so the plural's "<name> — <reason>"
  // shape would print it twice -- and a failure with no reason to give renders no line at
  // all, since a line that only restates the message is worse than none.
  const lines = one ? (f.reason ? [f.reason] : []) : (f.lines ?? []);
  showToast(
    message,
    // Hover text for the variant that has no lines, whose single line truncates. The view
    // drops the attribute once there are lines, which wrap and need no tooltip; what is
    // left to answer a pointer is the untruncated message, and the names the count hides.
    one ? message : joinNames(f.names),
    {
      assertive: f.assertive ?? false,
      lines,
      // The lines carry the whole reason and a reader cannot reach them: the toast expires
      // before it can be navigated to, so the announcement has to speak them.
      announce: [message, ...lines].join(". "),
      durationMs: lines.length > 0 ? READ_MS : undefined,
    },
  );
}

/** The outcome of one prepared start. Normalizes the bridge's two refusal shapes -- a
 * resolved `{ok:false}` and a rejection -- into one, so a caller branches on a single
 * thing. `pending` says the preparation is in flight and its outcome lands later, on
 * outputBinding.armFailed or multistream.changed. */
export interface StartOutcome {
  ok: boolean;
  pending: boolean;
  error: string;
}

/** One binding's prepared start: broadcast created and bound, ingest written back, then
 * the output started. THE start path for a destination joining a running broadcast. */
export async function startArmedOutput(uuid: string): Promise<StartOutcome> {
  try {
    const res = await obs.call("multistream.startOutput", { uuid });
    return res.ok
      ? { ok: true, pending: res.pending, error: "" }
      : { ok: false, pending: false, error: res.error };
  } catch (e) {
    return { ok: false, pending: false, error: (e as Error).message };
  }
}

// One wording for the detached-window refusal below, which is both spoken as a toast and
// thrown for the panel's own error strip to render.
const MID_STREAM_NEEDS_MAIN_WINDOW = "Arm this destination from the main window while streaming.";

/** Refuse a mid-stream arm out loud. Both refusals below need BOTH halves: the toast is
 * the only surface a detached dock has, and the throw is what puts the panel's optimistic
 * switch back rather than leaving it showing a state the host never took. Assertive on the
 * same terms as every other refusal of a control the user just operated. */
function refuseArm(message: string, detail: string): never {
  showToast(message, detail, { assertive: true });
  throw new Error(message);
}

// True while a mid-stream arm is between its persist and the modal actually mounting. For
// that window alone the panel is still clickable -- every later moment has the modal's
// backdrop over it -- and a second arm thrown inside it would open a second modal.
//
// Written ONLY on the mid-stream path, both directions of the assignment. Setting it from
// every call let a concurrent disable or a pre-live enable store `false` over a live arm's
// flag on its way through, so the guard waved the next arm past and the flag read as
// load-bearing while doing nothing.
//
// Best-effort, NOT an exclusion, and a counter would not make it one: the status refresh
// below awaits ahead of the guard, so two toggles thrown before the first poll resolves
// both suspend there and both read this as false. What makes that benign is the opener --
// it merges each seed into one set rather than replacing it -- so the outcome is a single
// modal holding both uuids. The guard earns its keep on the common path, where the store
// is already loaded and nothing suspends between the read and the write.
let midStreamArmInFlight = false;

/**
 * The destinations panels' enable/disable, for both the per-row switch and the
 * per-canvas master. The disable direction is unchanged everywhere: it stops that
 * output immediately, which is safe and instant and needs no validation step.
 *
 * The enable direction while a broadcast is running persists the flag and then opens
 * the Stream Information modal seeded with what it just armed, because the arm alone
 * sends nothing -- see the header. Pre-live it stays a plain persist: arming for the
 * next go-live is that switch's whole purpose there.
 *
 * In a detached dock the mid-stream enable is REFUSED rather than performed, because
 * that window cannot mount the dialog the arm depends on -- see the branch below.
 *
 * Rejections propagate so each panel keeps its own error surface and can put its
 * optimistic toggle back; the refusal above is thrown for the same reason, so the
 * switch snaps back instead of showing a state the host never took.
 */
export async function setDestinationsEnabled(uuids: string[], enabled: boolean): Promise<void> {
  // The status rows ARE the liveness answer, so an empty store reads as "nothing is
  // live". Before the first poll resolves that is not an answer, it is a gap, and a
  // toggle thrown inside it would let a mid-stream arm through as a plain pre-live one.
  // Closed at the one door both panels use rather than in each panel's mount path.
  if (enabled && !multistreamStatusStore.loaded) {
    await multistreamStatusStore.refresh();
  }
  // Read BEFORE the persist: the question is whether a broadcast was running when the
  // user threw the switch, not what the status re-poll this persist triggers settles on.
  // multistreamStatusStore.anyLive, not the global streaming flag: it is the one
  // predicate the modal's arm switch and its close-time revert also read, so no two
  // doors can classify the same destination differently.
  const midStream = enabled && multistreamStatusStore.anyLive;
  if (midStream && midStreamArmInFlight) {
    // Dropped rather than queued: the arm the user is waiting on is already on its way,
    // and the modal it opens renders every armed destination anyway -- including this one
    // once they flip it there, which is where a mid-stream arm belongs.
    refuseArm("Finish the destination you just armed first.", "Stream Information is opening for it now.");
  }
  // A detached dock is its own CEF window and mounts one dock, never the app shell, so
  // openGoLiveModal there sets a flag nothing renders: the destination would persist as
  // enabled, no dialog would appear, nothing would start, and -- because no modal ever
  // mounts -- nothing would switch it back off either. It would then join the NEXT
  // go-live unvalidated, which is the one outcome this whole path exists to prevent.
  //
  // So it refuses instead of arming. Refusing is only correct because there is no seam to
  // route this to the main window: every window.* bridge method acts on a window the
  // caller already owns (detach/redock/list/fullscreen/minimize/maximize/close) and none
  // focuses or opens a dialog in another. Give the host one and this branch becomes a
  // hand-off rather than a refusal -- but it must never become a silent fall-through.
  if (midStream && DETACHED_DOCK !== null) {
    refuseArm(MID_STREAM_NEEDS_MAIN_WINDOW, "Stream Information only opens in the main window.");
  }
  if (midStream) {
    midStreamArmInFlight = true;
  }
  try {
    await outputBindingStore.setEnabled(uuids, enabled);
  } finally {
    if (midStream) {
      midStreamArmInFlight = false;
    }
  }
  if (midStream) {
    openGoLiveModal("edit", uuids);
  }
}

// Arms this dialog has requested a start for and does not yet know the outcome of. An
// entry goes in BEFORE the call and comes out again the moment the answer makes it
// pointless -- refused, or up already -- so what is left is exactly the set still waiting
// on a `pending` preparation, which the host answers later on outputBinding.armFailed or
// through the status rows. The modal that asked is closed by then, so the tracking has to
// outlive it: module scope, not component state, for exactly that reason.
//
// Membership is also the ONLY thing that separates a failed mid-stream arm from a failed
// row Retry, which share the one event. A Retry never enters this set -- its destination
// was already out on this broadcast and stays armed either way -- so it is untouched by
// construction, and the row keeps the affordance the user just pressed.
const pendingArms = new Set<string>();

/** Take responsibility for a binding before its start is requested. Registered BEFORE the
 * call rather than after it because the stop edge below empties this set, and it can land
 * inside that await: adding afterwards would put a uuid into a set the stop had just
 * cleared, and a later armFailed would disarm a binding whose enabled flag legitimately
 * means "the next go-live" by then. */
export function trackPendingArm(uuid: string): void {
  pendingArms.add(uuid);
}

/** Hand it back: the start was refused, or it came up synchronously and there is no later
 * outcome to wait for. A no-op if the stop edge already cleared the set, which is the
 * point -- this must never re-add. */
export function untrackPendingArm(uuid: string): void {
  pendingArms.delete(uuid);
}

// Wired once, at module scope, and never torn down. This has to survive the modal that
// registered the arm, so hanging it off a component lifecycle would either miss the
// failure it exists to catch or stack a second listener on the next open.
//
// Nothing here can fire pre-live: an entry only ever arrives through trackPendingArm,
// which only the modal's mid-broadcast branch calls, and the stop edge below empties the
// set. So an arm made for the NEXT go-live is never in it and is never touched.
obs.on(EV.outputBindingArmFailed, (p) => {
  if (!pendingArms.delete(p.uuid)) {
    return;
  }
  // The arm was unvalidated and its preparation has now failed, so it goes back off on
  // the same rule the modal closes under: nothing armed-but-unvalidated survives. Best
  // effort -- a failed disarm leaves the flag set, which sends nothing until a go-live
  // and which the destination row reports as Standby.
  void outputBindingStore.setEnabled([p.uuid], false).catch(() => {});
});

obs.on(EV.multistreamChanged, (p) => {
  if (pendingArms.size === 0) {
    return;
  }
  // It came up, so the arm is validated by the thing it was waiting on and stops being
  // this set's business.
  for (const o of p.outputs) {
    if (isActiveState(o.state)) {
      pendingArms.delete(o.bindingUuid);
    }
  }
});

obs.on(EV.streamingChanged, (p) => {
  // Everything here belongs to a session that is over, and the mid-stream rule applies
  // to no part of it: these flags now describe the NEXT go-live, which is the one thing
  // an arm is always allowed to mean.
  if (!p.active) {
    pendingArms.clear();
  }
});

// Retries in flight, so a row's button can say so and a second click cannot stack a
// second start against the same binding.
const retrying = $state<Record<string, boolean>>({});

export function isRetrying(uuid: string): boolean {
  return !!retrying[uuid];
}

/**
 * Bring a destination that dropped off the running broadcast back onto it. No stream
 * info step, unlike a fresh mid-stream arm: its broadcast already exists and its info
 * was validated on the platform when it first went out, so the preparation edits in
 * place rather than creating.
 *
 * Deliberately does NOT call trackPendingArm, and that absence is load-bearing: this
 * destination's arm was never unvalidated, so a failure here must leave the flag alone
 * rather than switching off the row the user is trying to recover.
 */
export async function retryDestination(uuid: string, name: string): Promise<void> {
  if (retrying[uuid]) {
    return;
  }
  retrying[uuid] = true;
  try {
    const res = await startArmedOutput(uuid);
    // A pending preparation reports itself later on outputBinding.armFailed, which the
    // Studio page already surfaces; only the outright refusal is this call's to tell.
    if (!res.ok) {
      destinationFailureToast({ lead: "Couldn't start ", names: [name], reason: res.error, assertive: true });
    }
  } finally {
    delete retrying[uuid];
  }
}
