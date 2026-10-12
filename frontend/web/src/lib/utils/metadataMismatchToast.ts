// What a go-live actually carried, when it did not carry what was asked. Routed through the
// one shared toast surface, which the sibling refusal (streaming.startFailed) also uses.
//
// It also carries the parts of a push that did not land while the rest did
// (streaming.metadataWarning, such as a refused YouTube thumbnail). Those arrive on their own event,
// and often well after the mismatch rows, once a background retry gives up. They render here,
// beside the rows, rather than as a toast of their own: the surface holds one toast at a time, so
// two notices raised apart would each take the other down. Every push renders the whole current
// set, which also makes the order in which the two events land irrelevant.
//
// It names destinations and fields and prints no requested/actual values. Only cosmetic
// differences can reach here -- a safety disagreement (privacy/visibility) refuses the go-live
// via streaming.startFailed and never arrives -- so those values would only be a diff of text
// the streamer had just typed. A failure `reason` is different and is surfaced: on the hotkey,
// tray and scheduler paths there is no modal, so this toast is the only place it can appear.
//
// The three conditions stay apart in the wording. "The platform has something else", "the push
// never landed" and "we could not read the platform at all" lead to different actions, and this
// whole path exists to stop the app asserting a platform state it does not have.

import type { ObsEvents } from "$lib/api/bridge";
import { streamProfileStore } from "$lib/stores/streamProfileStore.svelte";
import { hideToast, showToast } from "$lib/stores/toastStore.svelte";
import { joinNames } from "$lib/utils/format";

/** One destination's entry in the streaming.metadataMismatch payload. Taken from the bridge
 *  contract rather than restated, so a field added on the C++ side cannot silently go missing
 *  from the type this file reasons about. */
export type MismatchRow = ObsEvents["streaming.metadataMismatch"]["destinations"][number];

/** One destination's streaming.metadataWarning: a part of its push that did not land. */
export type WarningRow = ObsEvents["streaming.metadataWarning"];

const KIND = "metadataMismatch";

// Longer than the 4s default: three clauses run to roughly thirty words, which is about nine
// seconds of reading, and this lands in the busiest moment the streamer has.
const DISMISS_MS = 9000;

// At most this many names (or field labels) are spelled out before the rest collapse into a
// count. Four destinations naming four fields each is the case that made the old block
// unreadable.
const NAME_LIMIT = 3;

// One clause per distinct field set is what keeps the sentence honest, but the count is bounded
// by the number of destinations, so a large rig could still produce a wall. Past this many the
// remainder becomes a count -- naming fewer destinations is a fair trade, asserting a pairing
// that isn't real is not.
const MAX_CLAUSES = 4;

// A provider error can run long, and this is a notification rather than a log viewer.
const REASON_MAX = 90;

// Invalidates an in-flight push whose profile names never arrived, so a retraction or a newer
// toast cannot be undone by a late resolution.
let pending = 0;

function uniq(items: string[]): string[] {
  return [...new Set(items)];
}

function short(s: string): string {
  return s.length > REASON_MAX ? s.slice(0, REASON_MAX - 1).trimEnd() + "…" : s;
}

// The Stream Information path and the warning event emit an empty label -- both run off the UI
// thread and cannot read the profile store that owns the name. Resolved here instead of guessed
// there.
function nameFor(row: { destination: string; profileUuid: string }): string {
  if (row.destination) {
    return row.destination;
  }
  const profile = streamProfileStore.byUuid(row.profileUuid);
  return profile?.targetName || profile?.label || "This destination";
}

// Collects destinations that share the same detail -- the same field set, or the same failure
// reason -- so a clause can never pair a destination with something belonging to another one.
// Unioning across rows instead would read "Twitch and YouTube have a different title and
// category" when Twitch differed only on title and YouTube only on category.
//
// Names are deduped per group because they are not unique: nameFor falls back to the literal
// "This destination" whenever a profile lookup misses, and the Stream Information path emits an
// empty label for every row, so two misses would otherwise read "This destination and This
// destination".
function groupRows(
  rows: MismatchRow[],
  detailOf: (r: MismatchRow) => string[],
): { detail: string[]; names: string[] }[] {
  const groups = new Map<string, { detail: string[]; names: string[] }>();
  for (const row of rows) {
    const detail = uniq(detailOf(row));
    // Sorted for identity only; `detail` keeps the payload's order for display.
    const key = [...detail].sort().join("\0");
    const found = groups.get(key);
    if (found) {
      found.names.push(nameFor(row));
    } else {
      groups.set(key, { detail, names: [nameFor(row)] });
    }
  }
  return [...groups.values()].map((g) => ({ detail: g.detail, names: uniq(g.names) }));
}

function clausesFor(rows: MismatchRow[], warnings: WarningRow[], streaming: boolean): string[] {
  // First: these are the ones with something to do about them. The sentence comes from the host,
  // which owns the cause and keeps the attempt count in it, so only the platform's reason is
  // shortened.
  const clauses = warnings.map(
    (w) => `${nameFor(w)}: ${w.message}${w.reason ? ` (${short(w.reason)})` : ""}`,
  );
  for (const g of groupRows(
    rows.filter((r) => r.fields.length > 0),
    (r) => r.fields.map((f) => f.label.toLowerCase()),
  )) {
    // A push answered before anything went live must not describe a stream as running.
    const verb = streaming
      ? g.names.length === 1
        ? "is live with"
        : "are live with"
      : g.names.length === 1
        ? "has"
        : "have";
    clauses.push(`${joinNames(g.names, NAME_LIMIT)} ${verb} a different ${joinNames(g.detail, NAME_LIMIT)}`);
  }
  for (const g of groupRows(
    rows.filter((r) => r.reason),
    (r) => [r.reason],
  )) {
    clauses.push(`Couldn't update ${joinNames(g.names, NAME_LIMIT)} — ${short(g.detail[0])}`);
  }
  const unconfirmed = uniq(rows.filter((r) => r.unconfirmed).map(nameFor));
  if (unconfirmed.length > 0) {
    clauses.push(`Couldn't check ${joinNames(unconfirmed, NAME_LIMIT)}`);
  }
  if (clauses.length <= MAX_CLAUSES) {
    return clauses;
  }
  const hidden = clauses.length - MAX_CLAUSES;
  return [...clauses.slice(0, MAX_CLAUSES), `${hidden} more difference${hidden === 1 ? "" : "s"} not shown`];
}

function push(
  rows: MismatchRow[],
  warnings: WarningRow[],
  streaming: boolean,
  onUserDismiss: () => void,
  onFocusReturn: () => void,
): void {
  const clauses = clausesFor(rows, warnings, streaming);
  // Rows carrying no fields, no reason and no unconfirmed describe nothing. Retract rather than
  // return: leaving the previous push standing would keep a notice up for state that is gone.
  if (clauses.length === 0) {
    hideMetadataMismatchToast();
    return;
  }
  const full = clauses.join(". ") + ".";
  // Announced politely and drawn like every other toast. Only cosmetic divergences reach here,
  // and alarm chrome on cosmetic content is what teaches a streamer to stop reading the channel.
  // The three states stay apart in the wording instead.
  showToast(clauses[0], full, {
    lines: clauses.slice(1),
    // The sentence itself, so a screen reader hears which destinations rather than a tally. The
    // toast is not keyboard-reachable inside its lifetime, so this is the only chance to say it.
    announce: full,
    dismissible: true,
    durationMs: DISMISS_MS,
    kind: KIND,
    onUserDismiss,
    onFocusReturn,
  });
}

/** Surface the accumulated rows and warnings for one go-live attempt, replacing any earlier push. */
export function showMetadataMismatchToast(
  rows: MismatchRow[],
  warnings: WarningRow[],
  streaming: boolean,
  hooks: { onUserDismiss: () => void; onFocusReturn: () => void },
): void {
  if (rows.length === 0 && warnings.length === 0) {
    hideMetadataMismatchToast();
    return;
  }
  // The names come from the profile store, which the C++ side cannot reach off the UI thread.
  // On the first go-live of a session the list may still be in flight; pushing now would name
  // every destination "This destination", so wait for it. start() is idempotent.
  streamProfileStore.start();
  const token = ++pending;
  if (streamProfileStore.loaded) {
    push(rows, warnings, streaming, hooks.onUserDismiss, hooks.onFocusReturn);
    return;
  }
  void streamProfileStore.whenReady().then(() => {
    if (token === pending) {
      push(rows, warnings, streaming, hooks.onUserDismiss, hooks.onFocusReturn);
    }
  });
}

/** Retract the notice once it no longer describes anything (stream stopped or go-live refused).
 *  Not the path a user dismissal takes -- that one also discards the rows. */
export function hideMetadataMismatchToast(): void {
  pending++;
  hideToast(KIND);
}
