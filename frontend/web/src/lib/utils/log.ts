// Gated, category-tagged web logger. Emits a structured `[<L>][<cat>]` prefix
// through console.* so the C++ Client::OnConsoleMessage parser can lift the level
// + category and route the line into the single session log file.
//
// `dbg` early-returns before building the message when the DEBUG gate is off, so
// the calls cost nothing on the default (quiet) path. info/warn/error always emit.
// The gate is the shared diagnostics store, seeded from diagnostics.get at boot and
// kept live by the debug.changed event. Until that seed lands the gate is unknown, and
// `dbg` holds its lines (up to EARLY_DBG_CAP) to write or drop once it is: boot is
// exactly when the early lines matter.

import { diagnosticsStore } from "$lib/stores/diagnosticsStore.svelte";
import type { LogCategory } from "$lib/utils/logCategories";

/** How many log.dbg lines are held before the gate is known; the oldest go first. */
export const EARLY_DBG_CAP = 500;

let early: { cat: LogCategory; a: unknown[] }[] | null = null;
let earlyDropped = 0;

function hold(cat: LogCategory, a: unknown[]): void {
  if (early === null) {
    early = [];
    diagnosticsStore.onGateKnown(() => {
      const held = early ?? [];
      const dropped = earlyDropped;
      early = null;
      earlyDropped = 0;
      if (!diagnosticsStore.debug) {
        return;
      }
      if (dropped > 0) {
        console.debug("[D][lifecycle]", `${dropped} earlier debug line(s) were not kept before the gate was read`);
      }
      for (const line of held) {
        console.debug(`[D][${line.cat}]`, ...line.a);
      }
    });
  }
  if (early.length >= EARLY_DBG_CAP) {
    early.shift();
    earlyDropped++;
  }
  early.push({ cat, a });
}

export const log = {
  dbg(cat: LogCategory, ...a: unknown[]): void {
    if (!diagnosticsStore.gateKnown) {
      hold(cat, a);
      return;
    }
    if (!diagnosticsStore.debug) {
      return;
    }
    console.debug(`[D][${cat}]`, ...a);
  },
  info(cat: LogCategory, ...a: unknown[]): void {
    console.info(`[I][${cat}]`, ...a);
  },
  warn(cat: LogCategory, ...a: unknown[]): void {
    console.warn(`[W][${cat}]`, ...a);
  },
  error(cat: LogCategory, ...a: unknown[]): void {
    console.error(`[E][${cat}]`, ...a);
  },
};
