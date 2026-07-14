// Gated, category-tagged web logger. Emits a structured `[<L>][<cat>]` prefix
// through console.* so the C++ Client::OnConsoleMessage parser can lift the level
// + category and route the line into the single session log file.
//
// `dbg` early-returns before building the message when the DEBUG gate is off, so
// the calls cost nothing on the default (quiet) path. info/warn/error always emit.
// The gate is the shared diagnostics store, seeded from diagnostics.get at boot and
// kept live by the debug.changed event.

import { diagnosticsStore } from "$lib/stores/diagnosticsStore.svelte";
import type { LogCategory } from "$lib/utils/logCategories";

export const log = {
  dbg(cat: LogCategory, ...a: unknown[]): void {
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
