// The alert box's three settings layers -- Defaults, then a per-event override, then the
// winning conditional variation -- and the one function that resolves them for an event.
// The overlay runtime and the editor's stage both import this module, so what the editor
// previews and what the stream shows cannot disagree.
//
// Data, not branches: an event's types, condition fields and built-in messages come from the
// template's scopes.json, and a condition field is one row in CONDITION_FIELDS.

import type { NormalizedEvent } from "../lib/api/bridge";
import { fmtCount, fmtMoney, fmtTally } from "../lib/utils/format";
import { isPlainObject } from "../lib/utils/plainObject";
import { platformName } from "../lib/theme/platformColors";
import { amountOf, MONEY_TYPES } from "./eventAmount";

/** The Defaults scope's id. Event scopes use their event key, variations their own id. */
export const DEFAULT_SCOPE = "default";

export interface AlertEventScope {
  key: string;
  label: string;
  /** The event types this scope answers for ("sub" covers sub, resub and subgift). */
  types: string[];
  /** The condition fields a variation of this event may test. */
  conditions: string[];
  /** The message an alert of this event shows when no scope sets `message`. */
  message: string;
  /** Built-in messages by platform, where a platform names the act differently: a YouTube
   * follow is a subscribe. Read before `message`, and only when no scope sets one. */
  platformMessages: Record<string, string>;
}

export interface VariationCondition {
  field: string;
  op: string;
  value: unknown;
  /** Money conditions only: the ISO 4217 code the amount is in. A Super Chat matches only in
   * that currency, as no exchange rate is applied. */
  currency?: string;
}

export interface AlertVariation {
  id: string;
  event: string;
  label?: string;
  when: VariationCondition;
  settings: Record<string, unknown>;
}

export interface AlertScopes {
  overrides: Record<string, Record<string, unknown>>;
  variations: AlertVariation[];
  events: AlertEventScope[];
}

export interface ResolvedAlert {
  /** Every field the alert renders with, after all three layers. */
  settings: Record<string, unknown>;
  /** The most specific scope that applied: a variation id, an event key, or "default". */
  scope: string;
  /** The event scope the event's type belongs to, or null for a type no scope declares. */
  eventKey: string | null;
}

export const CONDITION_OPS = [">=", "=="] as const;
export type ConditionOp = (typeof CONDITION_OPS)[number];

type ConditionReading = number | string | boolean | null;

interface ConditionFieldSpec {
  label: string;
  /** The value an event carries for this field, or null when it carries none. */
  read(e: NormalizedEvent): ConditionReading;
  /** Which ops the field supports. */
  ops: readonly ConditionOp[];
  /** What a condition's `value` must be for those ops. */
  valueKind: "number" | "tier" | "boolean" | "platform";
  /** The value as the rail shows it: "1,000 bits", "$5.00", "Tier 2", "12 months". */
  format(value: unknown, event: AlertEventScope | undefined, when?: VariationCondition): string;
}

const TIER_NUMBER = /^tier\s*(\d+)$/i;

/** "Tier 2" -> 2; a YouTube membership level name has no number and answers null. */
export function tierNumber(v: unknown): number | null {
  if (typeof v === "number" && Number.isFinite(v)) {
    return v;
  }
  const m = typeof v === "string" ? TIER_NUMBER.exec(v.trim()) : null;
  return m ? Number(m[1]) : null;
}

/** Whether an event scope's amounts are money, compared in hundredths of one currency. No
 * exchange rate is applied, so a money condition names its currency and matches only that. */
export function isMoneyScope(event: AlertEventScope | undefined): boolean {
  return !!event && event.types.some((t) => (MONEY_TYPES as ReadonlySet<string>).has(t));
}

const CURRENCY_CODE = /^[A-Z]{3}$/;

/** A condition's currency as an ISO 4217 code, or null when it names none or a malformed one. */
export function conditionCurrency(when: unknown): string | null {
  const c = isPlainObject(when) && typeof when.currency === "string" ? when.currency.trim().toUpperCase() : "";
  return CURRENCY_CODE.test(c) ? c : null;
}

const asNumber = (v: unknown): number => (typeof v === "number" ? v : Number(v));

export const CONDITION_FIELDS: Record<string, ConditionFieldSpec> = {
  amount: {
    label: "Amount",
    read: (e) => amountOf(e),
    ops: CONDITION_OPS,
    valueKind: "number",
    format: (v, event, when) =>
      isMoneyScope(event)
        ? fmtMoney(asNumber(v), conditionCurrency(when) ?? undefined)
        : fmtTally(event?.types[0] ?? "", asNumber(v)),
  },
  tier: {
    label: "Tier",
    read: (e) => e.tier ?? null,
    ops: CONDITION_OPS,
    valueKind: "tier",
    format: (v) => (typeof v === "number" ? `Tier ${v}` : String(v ?? "")),
  },
  months: {
    label: "Months",
    read: (e) => e.months ?? null,
    ops: CONDITION_OPS,
    valueKind: "number",
    format: (v) => `${fmtCount(asNumber(v))} months`,
  },
  count: {
    label: "Subs gifted",
    read: (e) => e.count ?? null,
    ops: CONDITION_OPS,
    valueKind: "number",
    format: (v) => `${fmtCount(asNumber(v))} gifted`,
  },
  gift: {
    label: "Gift",
    read: (e) => e.type === "subgift",
    ops: ["=="],
    valueKind: "boolean",
    format: (v) => (v === true ? "gift subs" : "not gifted"),
  },
  platform: {
    label: "Platform",
    read: (e) => e.platform ?? null,
    ops: ["=="],
    valueKind: "platform",
    format: (v) => `on ${platformName(String(v ?? ""))}`,
  },
};

/** Why a condition cannot be evaluated, or null when it can. A variation with a problem never
 * matches; the editor shows the reason beside it. `event` narrows to the fields that event
 * declares. */
export function conditionProblem(when: unknown, event?: AlertEventScope): string | null {
  if (!isPlainObject(when)) {
    return "The condition is missing.";
  }
  const field = typeof when.field === "string" ? when.field : "";
  const spec = Object.hasOwn(CONDITION_FIELDS, field) ? CONDITION_FIELDS[field] : undefined;
  if (!spec) {
    return `"${field}" is not a condition this alert box knows.`;
  }
  if (event && !event.conditions.includes(field)) {
    return `${event.label} alerts carry no ${spec.label.toLowerCase()}.`;
  }
  const op = when.op as ConditionOp;
  if (!spec.ops.includes(op)) {
    return `${spec.label} cannot be compared with "${String(when.op)}".`;
  }
  const v = when.value;
  if (spec.valueKind === "boolean" && typeof v !== "boolean") {
    return `${spec.label} must be yes or no.`;
  }
  if (spec.valueKind === "number" && !(typeof v === "number" && Number.isFinite(v))) {
    return `${spec.label} needs a number.`;
  }
  if (spec.valueKind === "platform" && (typeof v !== "string" || v.trim() === "")) {
    return "Platform needs a value.";
  }
  if (field === "amount" && isMoneyScope(event) && conditionCurrency(when) === null) {
    return "Choose the currency this amount is in.";
  }
  if (spec.valueKind === "tier") {
    if (op === ">=" && tierNumber(v) === null) {
      return 'Tier "at least" needs a numbered tier, such as 2.';
    }
    if (op === "==" && tierNumber(v) === null && (typeof v !== "string" || v.trim() === "")) {
      return "Tier needs a value.";
    }
  }
  return null;
}

/** Whether `v` matches `e`. A condition that cannot be evaluated, and an event carrying no
 * value for the field, both answer false. */
export function variationMatches(v: AlertVariation, e: NormalizedEvent): boolean {
  if (conditionProblem(v.when) !== null) {
    return false;
  }
  const spec = CONDITION_FIELDS[v.when.field];
  const reading = spec.read(e);
  if (reading === null) {
    return false;
  }
  const want = v.when.value;
  if (spec.valueKind === "boolean") {
    return reading === want;
  }
  if (spec.valueKind === "platform") {
    return String(reading).toLowerCase() === String(want).trim().toLowerCase();
  }
  // Money compares only within one currency: a condition without one, or a Super Chat in
  // another, never matches.
  if (v.when.field === "amount" && (MONEY_TYPES as ReadonlySet<string>).has(e.type)) {
    const currency = conditionCurrency(v.when);
    if (currency === null || (e.currency ?? "").toUpperCase() !== currency) {
      return false;
    }
  }
  if (spec.valueKind === "tier") {
    const have = tierNumber(reading);
    const wantN = tierNumber(want);
    if (v.when.op === ">=") {
      return have !== null && wantN !== null && have >= wantN;
    }
    return wantN !== null && have !== null
      ? have === wantN
      : String(reading).trim().toLowerCase() === String(want).trim().toLowerCase();
  }
  const have = Number(reading);
  return v.when.op === ">=" ? have >= (want as number) : have === want;
}

/** How a variation ranks against others that match: its threshold. A value without a number
 * (a membership level name, a boolean, a platform) ranks as 0. Money variations that match
 * one event share its currency, so their thresholds compare like for like. */
function thresholdOf(v: AlertVariation): number {
  const n = typeof v.when.value === "number" ? v.when.value : tierNumber(v.when.value);
  return n !== null && Number.isFinite(n) ? n : 0;
}

/** The matching variation of `eventKey` with the highest threshold; ties go to list order. */
export function winningVariation(
  variations: readonly AlertVariation[],
  eventKey: string,
  e: NormalizedEvent,
): AlertVariation | null {
  let best: AlertVariation | null = null;
  for (const v of variations) {
    if (v.event !== eventKey || !variationMatches(v, e)) {
      continue;
    }
    if (best === null || thresholdOf(v) > thresholdOf(best)) {
      best = v;
    }
  }
  return best;
}

export function eventScopeOf(events: readonly AlertEventScope[], type: string): AlertEventScope | null {
  return events.find((s) => s.types.includes(type)) ?? null;
}

/** The event's built-in message on `platform`: its platform message, else its message. */
export function builtInMessage(event: AlertEventScope, platform: string | undefined): string {
  const key = (platform ?? "").toLowerCase();
  return Object.hasOwn(event.platformMessages, key) ? event.platformMessages[key] : event.message;
}

/** Copy `layer`'s own fields over `into`. Whole values replace: an animation object set in a
 * scope is that scope's animation, not a patch onto the one below. */
function mergeLayer(into: Record<string, unknown>, layer: unknown): void {
  if (!isPlainObject(layer)) {
    return;
  }
  for (const key of Object.keys(layer)) {
    into[key] = layer[key];
  }
}

/** Defaults, then the event's override, then the winning variation, field by field. An empty
 * `message` after all three falls back to the event's built-in message for the event's
 * platform (builtInMessage). `defaults` is the
 * widget's resolved flat settings: every schema key at its override or its default. */
export function resolveAlertSettings(
  defaults: Record<string, unknown>,
  scopes: AlertScopes,
  e: NormalizedEvent,
): ResolvedAlert {
  const settings: Record<string, unknown> = { ...defaults };
  const event = eventScopeOf(scopes.events, e.type);
  if (!event) {
    return { settings, scope: DEFAULT_SCOPE, eventKey: null };
  }
  mergeLayer(settings, scopes.overrides[event.key]);
  const winner = winningVariation(scopes.variations, event.key, e);
  if (winner) {
    mergeLayer(settings, winner.settings);
  }
  if (typeof settings.message !== "string" || settings.message === "") {
    settings.message = builtInMessage(event, e.platform);
  }
  const hasOverride = isPlainObject(scopes.overrides[event.key]) && Object.keys(scopes.overrides[event.key]).length > 0;
  return { settings, scope: winner ? winner.id : hasOverride ? event.key : DEFAULT_SCOPE, eventKey: event.key };
}

/** The string-valued entries of `raw`, keys lower-cased; anything else is dropped. */
function stringMap(raw: unknown): Record<string, string> {
  const out: Record<string, string> = {};
  if (isPlainObject(raw)) {
    for (const [k, v] of Object.entries(raw)) {
      if (typeof v === "string") {
        out[k.toLowerCase()] = v;
      }
    }
  }
  return out;
}

/** Parse what the host injected (or overlays.get returned) into a well-formed AlertScopes.
 * Anything malformed is dropped rather than thrown on: a hand-edited overlays.json must not
 * take the alert box down. */
export function normalizeScopes(raw: unknown): AlertScopes {
  const src = isPlainObject(raw) ? raw : {};
  const overrides: Record<string, Record<string, unknown>> = {};
  if (isPlainObject(src.overrides)) {
    for (const [k, v] of Object.entries(src.overrides)) {
      if (isPlainObject(v)) {
        overrides[k] = v;
      }
    }
  }
  const variations: AlertVariation[] = [];
  if (Array.isArray(src.variations)) {
    for (const v of src.variations) {
      if (isPlainObject(v) && typeof v.id === "string" && typeof v.event === "string") {
        variations.push({
          id: v.id,
          event: v.event,
          label: typeof v.label === "string" ? v.label : undefined,
          when: (isPlainObject(v.when) ? v.when : {}) as unknown as VariationCondition,
          settings: isPlainObject(v.settings) ? v.settings : {},
        });
      }
    }
  }
  const events: AlertEventScope[] = [];
  if (Array.isArray(src.events)) {
    for (const s of src.events) {
      if (isPlainObject(s) && typeof s.key === "string") {
        events.push({
          key: s.key,
          label: typeof s.label === "string" ? s.label : s.key,
          types: Array.isArray(s.types) ? s.types.filter((t): t is string => typeof t === "string") : [s.key],
          conditions: Array.isArray(s.conditions) ? s.conditions.filter((c): c is string => typeof c === "string") : [],
          message: typeof s.message === "string" ? s.message : "",
          platformMessages: stringMap(s.platformMessages),
        });
      }
    }
  }
  return { overrides, variations, events };
}

// --- the editor's view of the same layers ----------------------------------------------

/** The scopes `scope` inherits from, nearest first: a variation inherits its event then
 * Defaults, an event inherits Defaults, Defaults inherits nothing. */
export function scopeChain(scopes: AlertScopes, scope: string): string[] {
  if (scope === DEFAULT_SCOPE) {
    return [];
  }
  const variation = scopes.variations.find((v) => v.id === scope);
  return variation ? [variation.event, DEFAULT_SCOPE] : [DEFAULT_SCOPE];
}

/** The settings a scope sets itself (its override map), or null for an unknown scope. */
export function scopeSettings(
  scopes: AlertScopes,
  defaults: Record<string, unknown>,
  scope: string,
): Record<string, unknown> | null {
  if (scope === DEFAULT_SCOPE) {
    return defaults;
  }
  const variation = scopes.variations.find((v) => v.id === scope);
  if (variation) {
    return variation.settings;
  }
  return scopes.events.some((e) => e.key === scope) ? (scopes.overrides[scope] ?? {}) : null;
}

/** What `key` would be in `scope` if the scope left it out, and which scope supplies it.
 * `defaults` is the Defaults scope's resolved values (override or schema default). An empty
 * message inherited all the way down reads as the event's built-in message, attributed to
 * Defaults: for a platform variation, that platform's built-in message. */
export function inheritedValue(
  scopes: AlertScopes,
  defaults: Record<string, unknown>,
  scope: string,
  key: string,
): { value: unknown; from: string } {
  for (const parent of scopeChain(scopes, scope)) {
    const layer = scopeSettings(scopes, defaults, parent);
    if (layer && Object.hasOwn(layer, key)) {
      const value = layer[key];
      if (key === "message" && value === "") {
        continue;
      }
      return { value, from: parent };
    }
  }
  if (key === "message") {
    const variation = scopes.variations.find((v) => v.id === scope);
    const event = scopes.events.find((e) => e.key === (variation ? variation.event : scope));
    const platform =
      variation?.when.field === "platform" && typeof variation.when.value === "string" ? variation.when.value : undefined;
    return { value: event ? builtInMessage(event, platform) : "", from: DEFAULT_SCOPE };
  }
  return { value: undefined, from: DEFAULT_SCOPE };
}

/** The label the editor shows for a scope: "Defaults", the event label, or the variation's. */
export function scopeLabel(scopes: AlertScopes, scope: string): string {
  if (scope === DEFAULT_SCOPE) {
    return "Defaults";
  }
  const variation = scopes.variations.find((v) => v.id === scope);
  if (variation) {
    return variation.label || conditionLabel(variation, scopes.events.find((e) => e.key === variation.event));
  }
  return scopes.events.find((e) => e.key === scope)?.label ?? scope;
}

const OP_TEXT: Record<ConditionOp, string> = { ">=": "≥", "==": "=" };

/** "≥ 1,000 bits"-style text for a variation's condition. */
export function conditionLabel(v: AlertVariation, event?: AlertEventScope): string {
  const spec = Object.hasOwn(CONDITION_FIELDS, v.when.field) ? CONDITION_FIELDS[v.when.field] : undefined;
  if (!spec) {
    return "Invalid condition";
  }
  const shown = spec.format(v.when.value, event, v.when);
  if (spec.valueKind === "boolean" || spec.valueKind === "platform") {
    return shown.charAt(0).toUpperCase() + shown.slice(1);
  }
  return `${OP_TEXT[v.when.op as ConditionOp] ?? String(v.when.op)} ${shown}`;
}
