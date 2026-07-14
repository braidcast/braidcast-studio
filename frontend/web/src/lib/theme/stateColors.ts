import type { MultistreamState, OutputStat } from "$lib/api/bridge";

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
};

/** MultistreamState plus the "off"/"disabled" aliases (an absent or disabled
 * binding renders like idle). Superset of STATE_COLOR for consumers that need it. */
export const STATE_COLOR_EXT: Record<MultistreamState | "off" | "disabled", string> = {
  ...STATE_COLOR,
  off: "var(--color-muted)",
  disabled: "var(--color-muted)",
};

/** Title-cased OutputStat.state -> the same tokens (Stats / Monitor). */
export const OUTPUT_STATE_COLOR: Record<OutputStat["state"], string> = {
  Idle: STATE_COLOR.idle,
  Connecting: STATE_COLOR.connecting,
  Live: STATE_COLOR.live,
  Error: STATE_COLOR.error,
};
