import type { MultistreamState, ScheduleState, TransportHealthState } from "$lib/api/bridge";

// Single source for the live-state -> token color mapping that was re-declared per
// consumer (StudioPage, CanvasDock, CanvasesPage, MultistreamDock, StatsDock,
// MonitorPage). Idle/off/disabled all read muted; the transient/error states carry
// the meter tokens so they re-skin with the active theme.

/** Lowercase MultistreamState -> dot color. */
export const STATE_COLOR: Record<MultistreamState, string> = {
  idle: "var(--color-muted)",
  connecting: "var(--meter-yellow)",
  live: "var(--meter-green)",
  error: "var(--color-live)",
  // Orange, mixed from the meter tokens (there is no orange theme axis): reads
  // between connecting (yellow) and error (red) and re-skins with every preset.
  reconnecting: "color-mix(in srgb, var(--meter-red) 50%, var(--meter-yellow))",
};

/** MultistreamState plus the "off"/"disabled" aliases (an absent or disabled
 * binding renders like idle). Superset of STATE_COLOR for consumers that need it. */
export const STATE_COLOR_EXT: Record<MultistreamState | "off" | "disabled", string> = {
  ...STATE_COLOR,
  off: "var(--color-muted)",
  disabled: "var(--color-muted)",
};

/** Keys of STATE_COLOR_EXT: the one vocabulary an edge color is allowed to speak. */
export type EdgeState = keyof typeof STATE_COLOR_EXT;

// The left edge already carries three meanings (transport health, platform identity,
// output state); a session's outcome and a planned entry's state borrow that same
// palette rather than adding a fourth. Both maps live here beside it so the history
// card and the calendar chip cannot drift into two readings of one edge.

/** sessions.end_reason -> the live-output-state key its edge borrows. */
export const SESSION_END_STATE: Record<string, EdgeState> = {
  crashed: "error",
  failed: "error",
  ended: "off",
};

/** The written label beside that color, so the state survives greyscale and a
 * color-blind reader -- seeing that a stream crashed is why the crash is recorded. */
export const SESSION_END_LABEL: Record<string, string> = {
  crashed: "Crashed",
  failed: "Failed",
  ended: "Ended",
};

/** ScheduleState -> the same palette. `armed` reads as connecting because that is
 * literally what it is: the entry is preparing to go live. `missed` reads as an
 * error because a broadcast that never happened is one. */
export const SCHEDULE_STATE_EDGE: Record<ScheduleState, EdgeState> = {
  planned: "off",
  armed: "connecting",
  live: "live",
  done: "off",
  missed: "error",
  canceled: "disabled",
};

/** The written label beside it, on the same terms as SESSION_END_LABEL. */
export const SCHEDULE_STATE_LABEL: Record<ScheduleState, string> = {
  planned: "Planned",
  armed: "Armed",
  live: "Live",
  done: "Done",
  missed: "Missed",
  canceled: "Canceled",
};

/** TransportHealthState -> the same token set as STATE_COLOR (G1: chat/events/
 * overlay transport health reuses the multistream status color language instead
 * of a second palette -- the state names differ, so this is a name remap only). */
export const TRANSPORT_STATE_COLOR: Record<TransportHealthState, string> = {
  connecting: STATE_COLOR.connecting,
  connected: STATE_COLOR.live,
  reconnecting: STATE_COLOR.reconnecting,
  failed: STATE_COLOR.error,
  disconnected: STATE_COLOR.idle,
};
