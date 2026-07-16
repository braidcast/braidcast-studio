import type { MultistreamState, TransportHealthState } from "$lib/api/bridge";

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
