// The default alert box as its tests load it: the shipped template files, and a stand-in for
// the OBSOverlay global that runs the REAL template filler and scope resolver -- so a test
// of what a card says exercises the same resolution the overlay runtime does. Animation and
// sound are inert: nothing here has a compositor or an AudioContext.

import { normalizeScopes, resolveAlertSettings } from "../src/overlay/alertScopes";
import { fillTemplate, fillTemplateParts, type TemplatePart } from "../src/overlay/fillTemplate";

const DIR = new URL("../public/overlay/default-alertbox/", import.meta.url);

export const ALERTBOX_SOURCE = await Bun.file(new URL("template.js", DIR)).text();
export const ALERTBOX_SCHEMA: { key: string; default?: unknown; scope?: string }[] = await Bun.file(
  new URL("fields.json", DIR),
).json();
export const ALERTBOX_SCOPES = normalizeScopes(await Bun.file(new URL("scopes.json", DIR)).json());
/** What the host injects for a stock widget with no overrides: each schema default. */
export const ALERTBOX_DEFAULTS: Record<string, unknown> = Object.fromEntries(
  ALERTBOX_SCHEMA.map((f) => [f.key, f.default]),
);

export interface StubScopes {
  overrides?: Record<string, Record<string, unknown>>;
  variations?: unknown[];
}

/** An OBSOverlay for the alert box. `extra` replaces any member, which is how a test captures
 * the handlers the template registers or fixes what a formatter returns. */
export function alertboxOverlay(
  fields: Record<string, unknown> = ALERTBOX_DEFAULTS,
  scopes: StubScopes = {},
  extra: Record<string, unknown> = {},
): Record<string, unknown> {
  const resolved = normalizeScopes({ ...scopes, events: ALERTBOX_SCOPES.events });
  return {
    fields,
    onLoad() {},
    onEvent() {},
    onEventRedaction() {},
    formatAmount: () => "",
    formatAmountText: () => "",
    formatCount: (n: number) => String(n),
    fillTemplate,
    fillTemplateParts,
    textField: (f: Record<string, unknown>, k: string, d: string) => (f[k] != null ? String(f[k]) : d),
    resolveAlert: (e: Parameters<typeof resolveAlertSettings>[2]) => resolveAlertSettings(fields, resolved, e),
    animate: () => null,
    animationMs: () => 0,
    applyTextFx: (el: { textContent: string }, parts: TemplatePart[]) => {
      el.textContent = parts.map((p) => p.text).join("");
      return [];
    },
    logOnce() {},
    playSound() {},
    ...extra,
  };
}
