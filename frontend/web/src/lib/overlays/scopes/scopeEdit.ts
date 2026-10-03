// Edits to a scoped widget's three layers, as pure functions over the stored shape: Defaults
// in `settings`, per-event maps in `overrides`, conditional `variations`. The editor routes
// every write through here, so "which map does this scope's edit land in" is decided once.

import type { OverlayField } from "$lib/api/bridge";
import {
  CONDITION_FIELDS,
  DEFAULT_SCOPE,
  isMoneyScope,
  scopeSettings,
  type AlertEventScope,
  type AlertVariation,
  type VariationCondition,
} from "../../../overlay/alertScopes";

/** The stored layers, as the widget document carries them. */
export interface ScopeLayers {
  settings: Record<string, unknown>;
  overrides: Record<string, Record<string, unknown>>;
  variations: AlertVariation[];
}

/** The map `scope` stores its own values in: the resolver's own lookup, over the stored
 * layers. An event with no override yet reads as empty; so does an unknown scope, and a
 * write to one is dropped (see withLayer). */
export function layerOf(
  layers: ScopeLayers,
  scope: string,
  events: readonly AlertEventScope[],
): Record<string, unknown> {
  return scopeSettings({ ...layers, events: [...events] }, layers.settings, scope) ?? {};
}

/** The layers after replacing `scope`'s own map. An event override emptied by a reset is
 * removed outright, so the rail's "has an override" dot goes out with it. */
export function withLayer(
  layers: ScopeLayers,
  scope: string,
  events: readonly AlertEventScope[],
  next: Record<string, unknown>,
): ScopeLayers {
  if (scope === DEFAULT_SCOPE) {
    return { ...layers, settings: next };
  }
  if (layers.variations.some((v) => v.id === scope)) {
    return { ...layers, variations: layers.variations.map((v) => (v.id === scope ? { ...v, settings: next } : v)) };
  }
  if (!events.some((e) => e.key === scope)) {
    return layers;
  }
  const overrides = { ...layers.overrides };
  if (Object.keys(next).length === 0) {
    delete overrides[scope];
  } else {
    overrides[scope] = next;
  }
  return { ...layers, overrides };
}

/** Whether a field is editable in `scope`: widget-scope fields live in Defaults alone. */
export function fieldInScope(field: OverlayField, scope: string): boolean {
  return scope === DEFAULT_SCOPE || field.scope !== "widget";
}

/** The condition a new variation starts with, per field: a value a streamer would plausibly
 * want and can edit straight away. YouTube is the platform a follow variation is usually
 * for: its follow is a subscribe. */
const STARTING_VALUE: Record<string, unknown> = {
  amount: 1000,
  tier: 2,
  months: 12,
  count: 5,
  gift: true,
  platform: "youtube",
};

/** The currency a new money condition starts in. The editor's currency field changes it. */
export const STARTING_CURRENCY = "USD";

/** A variation id: "v_" and four hex digits, unique within the widget. Short because it names
 * the variation's uploads (v_8f2c-media.webm), and it must survive SanitizeAssetKey as is. */
export function newVariationId(taken: readonly string[], random: () => number = Math.random): string {
  for (;;) {
    const id = "v_" + Math.floor(random() * 0x10000).toString(16).padStart(4, "0");
    if (!taken.includes(id)) {
      return id;
    }
  }
}

/** The condition a variation starts with when it tests `field`, or null for a field the
 * resolver does not know. Also what the condition editor resets to on a field change, since
 * a value meant for one field (1000 bits) is rarely sensible for another (1000 months). An
 * amount of `event` that is money names its currency, or it would never match. */
export function startingCondition(field: string, event?: AlertEventScope): VariationCondition | null {
  if (!Object.hasOwn(CONDITION_FIELDS, field)) {
    return null;
  }
  const when: VariationCondition = { field, op: CONDITION_FIELDS[field].ops[0], value: STARTING_VALUE[field] };
  return field === "amount" && isMoneyScope(event) ? { ...when, currency: STARTING_CURRENCY } : when;
}

/** A new variation of `event`, testing the first condition it declares, or null for an event
 * that declares none: there is nothing to vary on. */
export function newVariation(
  event: AlertEventScope,
  taken: readonly string[],
  random?: () => number,
): AlertVariation | null {
  const when = event.conditions.map((field) => startingCondition(field, event)).find((c) => c !== null);
  return when ? { id: newVariationId(taken, random), event: event.key, when, settings: {} } : null;
}

/** The layers without variation `id`, plus what an undo needs to put it back where it was. */
export function withoutVariation(
  layers: ScopeLayers,
  id: string,
): { layers: ScopeLayers; removed: AlertVariation | null; index: number } {
  const index = layers.variations.findIndex((v) => v.id === id);
  if (index < 0) {
    return { layers, removed: null, index: -1 };
  }
  const variations = layers.variations.filter((v) => v.id !== id);
  return { layers: { ...layers, variations }, removed: layers.variations[index], index };
}

/** The layers with `v` put back at `index`, the undo of withoutVariation. */
export function withVariationAt(layers: ScopeLayers, v: AlertVariation, index: number): ScopeLayers {
  const variations = [...layers.variations];
  variations.splice(Math.min(Math.max(index, 0), variations.length), 0, v);
  return { ...layers, variations };
}

/** The layers with variation `id`'s condition or label replaced. */
export function withVariationMeta(
  layers: ScopeLayers,
  id: string,
  patch: { when?: VariationCondition; label?: string },
): ScopeLayers {
  return {
    ...layers,
    variations: layers.variations.map((v) => {
      if (v.id !== id) {
        return v;
      }
      const next: AlertVariation = { ...v, ...(patch.when ? { when: patch.when } : {}) };
      if (patch.label !== undefined) {
        if (patch.label.trim() === "") {
          delete next.label;
        } else {
          next.label = patch.label;
        }
      }
      return next;
    }),
  };
}
