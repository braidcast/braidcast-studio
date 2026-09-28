import type { OAuthProvider, OAuthProviderField, StreamInfoPreset } from "$lib/api/bridge";
import {
  ALL_LAYER,
  fieldScope,
  inheritLayers,
  isBlankVal,
  isEmptyVal,
  isListType,
  isPerDestination,
  resolveRequiredEnum,
  slotKey,
  valuesEqual,
} from "$lib/dialogs/golive/fieldValue";

// What a preset carries, and where each value belongs, held once for both surfaces that
// load one -- the Go Live modal (which writes into its layers) and the schedule entry
// editor (which writes into its flat per-destination rows). Two hand-written copies of
// this rule would be two answers to "does this field belong in a preset", and the answer
// decides whether applying a sheet silently re-addresses a destination.
//
// A preset carries EVERY declared field except one that addresses a destination
// (isPerDestination -- which Page a stream posts to): re-applying a sheet must never
// repoint a stream, so such a value stays on the destination it names. Everything else is
// what saving "all the details" means, and it lands in the two bags the host keeps:
//
//  - `shared` holds a cross-provider field (scope "all": title, description) that every
//    armed provider declaring it agreed on.
//  - `byProvider[p]` holds every provider-scoped and channel-scoped value of provider p,
//    and a cross-provider field when providers differed about it -- each provider's own
//    value, "" for one that held none, rather than nothing. A channel-scoped value (latency,
//    made for kids, Twitch's language) is kept once per provider and applied to every
//    channel of that provider. The bag also carries PRESET_FORMAT_KEY, which is how a load
//    tells a provider this sheet covered from one it predates.
//
// A provider's own value outranks `shared`, read through presetValueOf by every caller.
// The one thing a preset cannot keep is two channels of the SAME provider disagreeing on
// a field, since a bag holds one value per provider; collectPreset names those instead.

/** Marks a provider bag written by this format. Its presence is what says the sheet was
 * saved with that provider armed and could hold its channel-scoped fields, so a missing one
 * is something the sheet stated; a bag without it predates channel-scoped fields travelling
 * at all, so their absence there says nothing. Never a field key -- descriptor keys are
 * developer-owned and none starts with an underscore -- so no reader mistakes it for one, and
 * the host's identity walks its field table alone, so it never forks a preset. */
export const PRESET_FORMAT_KEY = "__v";
export const PRESET_FORMAT = 2;

/** The two bags exactly as the host stores them. */
export interface PresetSheet {
  shared: Record<string, unknown>;
  byProvider: Record<string, Record<string, unknown>>;
}

/** One value a preset holds for one declared field. */
export interface PresetFieldValue {
  field: OAuthProviderField;
  value: unknown;
  /** True when it came from the provider's own bag rather than `shared`. */
  own: boolean;
}

/** The value `sheet` holds for `field` of `providerId`, or undefined when it holds none.
 * The provider's own bag first, then `shared` -- and `shared` only for a cross-provider
 * field, so a hand-edited sheet with a category in it cannot hand one platform's category
 * id to another. Tolerates a row whose bags are missing, like presetLabel. */
function presetValueOf(
  sheet: PresetSheet,
  field: OAuthProviderField,
  providerId: string,
): PresetFieldValue | undefined {
  const own = sheet.byProvider?.[providerId]?.[field.key];
  if (!isEmptyVal(field.type, own)) {
    return { field, value: own, own: true };
  }
  const shared = sheet.shared?.[field.key];
  if (fieldScope(field) !== "all" || isEmptyVal(field.type, shared)) {
    return undefined;
  }
  return { field, value: shared, own: false };
}

/** The value a field shows once no layer holds one: a required enum's resolved default,
 * a declared default (made for kids: false), or off for a flag that declares none. What a
 * reset lands on, and what an untouched form collects. A stated empty list is none of
 * these -- it is an intent ("send none"). */
function isAtRestValue(field: OAuthProviderField, value: unknown): boolean {
  const required = resolveRequiredEnum(field, "");
  if (required !== "") {
    return valuesEqual(field.type, value, required);
  }
  if (field.type === "bool") {
    return value === (field.default ?? false);
  }
  return field.default !== undefined && valuesEqual(field.type, value, field.default);
}

/** What a reset writes: the resting value where the field has one to send -- a required
 * choice's default, a flag's default or off, an empty list -- else nothing. Only ever
 * written for a `clearable` field, whose push acts on exactly this value. */
function resetValue(field: OAuthProviderField): unknown {
  if (isListType(field.type)) {
    return [];
  }
  if (field.type === "bool") {
    return field.default ?? false;
  }
  return resolveRequiredEnum(field, "") || field.default;
}

/** Is this bag one the current format wrote? */
function isCurrentFormat(bag: Record<string, unknown> | undefined): boolean {
  return bag?.[PRESET_FORMAT_KEY] === PRESET_FORMAT;
}

/** The declared fields of `provider` a preset may carry at all, whether or not this one
 * holds a value for them. */
export function carriableFields(provider: OAuthProvider): OAuthProviderField[] {
  return provider.fields.filter((f) => !isPerDestination(f));
}

/** The declared fields of `provider` that no preset can carry -- each addresses one
 * destination, so it stays where it is. */
export function uncarriableFields(provider: OAuthProvider): OAuthProviderField[] {
  return provider.fields.filter(isPerDestination);
}

/** What this preset actually states for `provider`: one entry per carriable field the
 * preset holds a value for, skipping every field it says nothing about. What silence then
 * means is the applier's call (see presetOutcome); the schedule editor leaves it alone.
 *
 * A stated empty tag list is not silence — isEmptyVal keeps the two apart for that type —
 * so a sheet saved from a channel the user deliberately cleared re-applies the clear
 * rather than quietly leaving the next channel's tags standing. */
export function presetValuesFor(preset: StreamInfoPreset, provider: OAuthProvider): PresetFieldValue[] {
  const out: PresetFieldValue[] = [];
  for (const field of carriableFields(provider)) {
    const held = presetValueOf(preset, field, provider.id);
    if (held) {
      out.push(held);
    }
  }
  return out;
}

/** The label a preset reads by: its own name, else the title it carries (shared first,
 * then any provider's own), else a stand-in -- a row with no words on it is unpickable. */
export function presetLabel(preset: StreamInfoPreset): string {
  const named = (preset.name ?? "").trim();
  if (named !== "") {
    return named;
  }
  // Tolerates a row whose bags are missing. This runs while the picker renders each row,
  // so a throw here takes the whole dialog down and leaves it unable to reopen -- a
  // disproportionate answer to one unreadable preset among many readable ones.
  const bags = [preset.shared, ...Object.values(preset.byProvider ?? {})];
  for (const bag of bags) {
    const title = bag?.["title"];
    if (typeof title === "string" && title.trim() !== "") {
      return title.trim();
    }
  }
  return "Untitled preset";
}

/** A field one of the connected providers declares but no preset can carry, named for a
 * sentence. Deduped across providers by the platform + field pair. */
export interface UncarriedField {
  providerName: string;
  label: string;
}

/** What applying a preset leaves untouched, across the providers actually in play: the
 * fields that address a destination. Computed from the descriptors, so a provider that
 * declares one changes this sentence without an edit here, and none leaves it empty. */
export function uncarriedFields(providers: OAuthProvider[]): UncarriedField[] {
  const seen = new Set<string>();
  const out: UncarriedField[] = [];
  for (const p of providers) {
    for (const f of uncarriableFields(p)) {
      const key = slotKey(p.id, f.key);
      if (!seen.has(key)) {
        seen.add(key);
        out.push({ providerName: p.displayName, label: f.label });
      }
    }
  }
  return out;
}

/** A plain two-level bag write: `bags[outer][key] = value`, removing the key for undefined.
 * Replaces the inner bag rather than mutating it, which is what the Go Live modal's state
 * needs to see the change. */
export function putBagValue(
  bags: Record<string, Record<string, unknown>>,
  outer: string,
  key: string,
  value: unknown,
): void {
  const next = { ...(bags[outer] ?? {}) };
  if (value === undefined) {
    delete next[key];
  } else {
    next[key] = value;
  }
  bags[outer] = next;
}

// ---- Go Live: what one load does to every channel the modal renders -------------

/** What one field of one provider becomes on a load. */
type PresetOutcome = { kind: "set"; value: unknown } | { kind: "reset"; value: unknown } | { kind: "keep" };

/** The providers a sheet was saved from, as far as it can tell: every provider it keeps a bag
 * for. The current format writes one for each armed provider; an older sheet only for a
 * provider that held something of its own, so there this is a lower bound. */
function sheetProviders(preset: StreamInfoPreset): string[] {
  return Object.keys(preset.byProvider ?? {});
}

/** May this preset's silence about `field` reset it on `providerId`'s channels?
 *
 * Only where a reset is true on BOTH sides and cannot be a guess:
 *  - never a safety field: a load that says nothing about privacy or made for kids must not
 *    move who can see the broadcast, whichever way the resting value points (Facebook's
 *    rests at public);
 *  - only a `clearable` one, whose Go Live push of the reset value leaves the new broadcast
 *    holding what the blank form shows -- blanking any other in the form would send the
 *    platform something else;
 *  - and only where the sheet could have held the field. A shared field could when some
 *    provider the sheet was saved from declares it (`declaredBy`, read from every provider
 *    this build knows, not from the channels open now): a sheet saved from Twitch alone
 *    never had a description to leave out, while one saved from YouTube did, even when only
 *    a Facebook channel is loading it. A provider-scoped one could when the sheet
 *    has a bag for this provider at all -- one that was armed and held anything. A
 *    channel-scoped one only when that bag is in the current format: an older bag had no
 *    place for channel fields, so their absence there states nothing. */
function mayReset(
  preset: StreamInfoPreset,
  field: OAuthProviderField,
  providerId: string,
  declaredBy: (providerId: string, key: string) => boolean,
): boolean {
  if (field.safety === true || field.clearable !== true) {
    return false;
  }
  const bag = preset.byProvider?.[providerId];
  switch (fieldScope(field)) {
    case "all":
      return sheetProviders(preset).some((p) => declaredBy(p, field.key));
    case "provider":
      return bag !== undefined && bag !== null;
    default:
      return isCurrentFormat(bag);
  }
}

/** One field of one provider on a load: the preset's value, a reset (see mayReset), or
 * left exactly as the channel shows it now. */
function presetOutcome(
  preset: StreamInfoPreset,
  field: OAuthProviderField,
  providerId: string,
  declaredBy: (providerId: string, key: string) => boolean,
): PresetOutcome {
  const held = presetValueOf(preset, field, providerId);
  if (held) {
    return { kind: "set", value: held.value };
  }
  return mayReset(preset, field, providerId, declaredBy)
    ? { kind: "reset", value: resetValue(field) }
    : { kind: "keep" };
}

/** Do two values show the same thing? Empty and empty agree; a value and an empty never do
 * -- valuesEqual alone reads a missing list as [] and a stated [] as the same, which would
 * let a stated clear dissolve into "inherit". */
function sameShown(type: string, a: unknown, b: unknown): boolean {
  const aEmpty = isEmptyVal(type, a);
  const bEmpty = isEmptyVal(type, b);
  return aEmpty || bEmpty ? aEmpty && bEmpty : valuesEqual(type, a, b);
}

/** One channel the modal renders, as a load takes it. */
export interface PresetChannel {
  provider: OAuthProvider;
  accountId: string;
  /** Every stream on this channel: a value the load decides clears the key's per-stream
   * override as well, or it would be outranked and read as inert. */
  profileUuids: string[];
  /** Armed for this go-live. Every channel is loaded into -- the inherit layers are shared
   * with the disarmed ones anyway -- but only an armed channel's resets are named. */
  armed: boolean;
  /** Its effective channel-level values BEFORE the load: what a kept field is held at, and
   * what a reset is measured against when it is named. */
  resolved: Record<string, unknown>;
}

/** The Go Live modal's layers, mutated in place by a load. */
export interface PresetApplyState {
  layerValues: Record<string, Record<string, unknown>>;
  channelValues: Record<string, Record<string, unknown>>;
  streamOverrides: Record<string, Record<string, unknown>>;
  streamOverrideOn: Record<string, boolean>;
}

export interface PresetApplyResult {
  /** slotKey(bucket, key) for every inherit layer written. Prefill must not reseed them. */
  touchedLayers: string[];
  /** slotKey(accountId, key) for every channel key the load set or reset. Prefill must
   * neither refill one nor restore a remembered stream override of it. A key the load only
   * held where it was (a field it could not reset) is not in here: the bucket it sits under
   * is in touchedLayers, and a stream's own remembered value of it is still the user's. */
  touchedChannels: string[];
  /** The resets an armed channel can see, named for a sentence. */
  resets: string[];
}

/** Load `preset` into the modal's layers, per field and never wholesale.
 *
 * Each (provider, field) gets one outcome (presetOutcome). A field with an inherit bucket
 * is decided for every channel reading that bucket together: the bucket takes the value the
 * preset states for it -- `shared` for the cross-provider one, the provider's outcome for a
 * provider's own -- and each channel then inherits it or, where its own outcome differs
 * (a provider's own title, a field kept because it could not be cleared), holds that value
 * in its own bag. A bucket no channel's outcome touches is not written at all. A field with
 * no bucket (channel-scoped) lands in each channel's own bag. Either way a decided key is
 * cleared from the channel's streams, or a stream override would outrank it.
 *
 * Every channel in `channels` is loaded into, armed or not: the buckets are shared, so a
 * disarmed channel sees them anyway, and a channel whose key was cleared must end at the
 * preset's value for its provider rather than blank.
 *
 * `providers` is every provider descriptor this build knows (oauth.providers), which is
 * where what the sheet's own providers declare is read from -- a provider the sheet was
 * saved from need not have a channel open now.
 *
 * A stream's override switch goes off only when THIS load took its last override away --
 * a switch left on over nothing reads "on" beside "0 overrides", while one already on with
 * none is the user's own state. `hasOverrides` is the modal's own count of a stream's
 * divergences, which knows which keys are addresses rather than overrides. */
export function applyPresetTo(
  state: PresetApplyState,
  preset: StreamInfoPreset,
  channels: PresetChannel[],
  providers: OAuthProvider[],
  hasOverrides: (provider: OAuthProvider, bag: Record<string, unknown>) => boolean,
): PresetApplyResult {
  const touchedLayers = new Set<string>();
  const touchedChannels = new Set<string>();
  const hadOverrides = new Map<string, boolean>();
  // A provider the sheet names that this build does not know proves nothing either way.
  const descriptors = new Map(providers.map((p) => [p.id, p]));
  const declaredBy = (providerId: string, key: string) => {
    const provider = descriptors.get(providerId);
    return provider !== undefined && carriableFields(provider).some((f) => f.key === key);
  };
  for (const c of channels) {
    for (const uuid of c.profileUuids) {
      hadOverrides.set(uuid, hasOverrides(c.provider, state.streamOverrides[uuid] ?? {}));
    }
  }

  // `decided` is false for a key only held where it was: its channel value is pinned so
  // the bucket moving under it changes nothing, and its streams keep what they had.
  const decide = (c: PresetChannel, key: string, value: unknown, decided: boolean) => {
    putBagValue(state.channelValues, c.accountId, key, value);
    if (decided) {
      touchedChannels.add(slotKey(c.accountId, key));
      for (const uuid of c.profileUuids) {
        if (state.streamOverrides[uuid] && key in state.streamOverrides[uuid]) {
          putBagValue(state.streamOverrides, uuid, key, undefined);
        }
      }
    }
  };

  interface Entry {
    c: PresetChannel;
    field: OAuthProviderField;
    outcome: PresetOutcome;
  }
  const byBucket = new Map<string, { bucket: string; entries: Entry[] }>();
  const outcomes: Entry[] = [];
  for (const c of channels) {
    for (const field of carriableFields(c.provider)) {
      const outcome = presetOutcome(preset, field, c.provider.id, declaredBy);
      outcomes.push({ c, field, outcome });
      const bucket = inheritLayers(field, c.provider.id)[0];
      if (bucket === undefined) {
        if (outcome.kind !== "keep") {
          decide(c, field.key, outcome.value, true);
        }
        continue;
      }
      const slot = slotKey(bucket, field.key);
      const group = byBucket.get(slot) ?? { bucket, entries: [] };
      group.entries.push({ c, field, outcome });
      byBucket.set(slot, group);
    }
  }

  for (const [slot, { bucket, entries }] of byBucket) {
    const moving = entries.find((e) => e.outcome.kind !== "keep");
    if (!moving) {
      continue;
    }
    const { field } = moving;
    const layer =
      bucket === ALL_LAYER
        ? presetValueOf({ shared: preset.shared, byProvider: {} }, field, moving.c.provider.id)?.value
        : (moving.outcome as { value: unknown }).value;
    putBagValue(state.layerValues, bucket, field.key, layer);
    touchedLayers.add(slot);
    for (const { c, outcome } of entries) {
      const target = outcome.kind === "keep" ? c.resolved[field.key] : outcome.value;
      decide(c, field.key, sameShown(field.type, target, layer) ? undefined : target, outcome.kind !== "keep");
    }
  }

  for (const c of channels) {
    for (const uuid of c.profileUuids) {
      if (hadOverrides.get(uuid) && !hasOverrides(c.provider, state.streamOverrides[uuid] ?? {})) {
        state.streamOverrideOn[uuid] = false;
      }
    }
  }

  return {
    touchedLayers: [...touchedLayers],
    touchedChannels: [...touchedChannels],
    resets: resetNames(outcomes),
  };
}

/** The resets an armed channel can see: a reset outcome whose field the channel was
 * showing something other than the reset value for. A cross-provider field reads by its
 * label alone when every armed provider declaring it was reset, since it then went
 * everywhere; otherwise each reset names its platform, as one a platform cannot clear
 * (Facebook's description) stays put beside it. */
function resetNames(
  outcomes: { c: PresetChannel; field: OAuthProviderField; outcome: PresetOutcome }[],
): string[] {
  const armed = outcomes.filter((o) => o.c.armed);
  const notReset = new Set(armed.filter((o) => o.outcome.kind !== "reset").map((o) => o.field.key));
  const names: string[] = [];
  for (const { c, field, outcome } of armed) {
    if (outcome.kind !== "reset") {
      continue;
    }
    const was = c.resolved[field.key];
    if (isBlankVal(field.type, was) || isAtRestValue(field, was) || sameShown(field.type, was, outcome.value)) {
      continue;
    }
    const alone = fieldScope(field) === "all" && !notReset.has(field.key);
    const name = alone ? field.label : `${field.label} (${c.provider.displayName})`;
    if (!names.includes(name)) {
      names.push(name);
    }
  }
  return names;
}

/** Did a load set or reset this channel key? The one reading of applyPresetTo's
 * `touchedChannels` for the modal's prefill: a key it decided is neither refilled from a
 * remembered channel value nor buried under a remembered stream override. */
export function presetDecided(decided: ReadonlySet<string>, accountId: string, key: string): boolean {
  return decided.has(slotKey(accountId, key));
}

/** A remembered stream bag with every key a load decided for its channel taken out, so
 * restoring it cannot outrank the preset. A key the load only held stays: the stream's own
 * value of it is still the user's. */
export function withoutPresetDecided(
  bag: Record<string, unknown> | null | undefined,
  accountId: string,
  decided: ReadonlySet<string>,
): Record<string, unknown> {
  return Object.fromEntries(Object.entries(bag ?? {}).filter(([key]) => !presetDecided(decided, accountId, key)));
}

// ---- Saving: one sheet from the armed channels ------------------------------------

/** One channel's resolved values, as the Go Live modal already computes them for a push. */
export interface PresetSource {
  provider: OAuthProvider;
  /** The channel's effective fields (every layer merged, empties already dropped). */
  resolved: Record<string, unknown>;
}

export interface CollectedPreset {
  sheet: PresetSheet;
  /** "Label (Platform)" for every field the sheet deliberately does NOT carry for that
   * platform, because two of its channels held different values and a bag holds one. */
  conflicts: string[];
}

/** The sheet to store: the resolved value of every carriable field, sorted into the same
 * two bags applying reads back out, with every armed provider's bag marked as this format.
 *
 * `sources` is one entry per armed ACCOUNT, so several can feed one provider's slot. Within
 * a provider a blank channel defers to one holding a value -- the provider's channels then
 * share it, as they do in the modal. Two channels holding DIFFERENT values have no answer
 * that is not a guess -- keeping either would save half of this go-live's stream info with
 * nothing saying which half -- so that provider's value is left out and named in
 * `conflicts`.
 *
 * Providers differing from EACH OTHER is not a conflict: each has its own bag. A
 * cross-provider field every armed provider declaring it held alike goes in `shared`, the
 * ordinary case of one title across every destination. Otherwise -- different values, or
 * one provider holding none -- each provider's value goes in its own bag, "" for the one
 * that held none (such a field is free text by its scope), and `shared` holds nothing, so a
 * load hands no provider a value it did not have. That also holds when one provider's
 * channels conflicted: sharing the others' value would give it a value neither of its
 * channels held.
 *
 * Symmetry with presetValuesFor therefore holds per provider and key: every value that
 * survives is the one presetValuesFor hands back for that provider. */
export function collectPreset(sources: PresetSource[]): CollectedPreset {
  interface Slot {
    field: OAuthProviderField;
    provider: OAuthProvider;
    /** undefined while every channel of the provider has held nothing. */
    value: unknown;
    agreed: boolean;
  }
  const sheet: PresetSheet = { shared: {}, byProvider: {} };
  // One slot per provider and key: what that provider's channels hold, and whether they agree.
  const slots = new Map<string, Slot>();
  for (const { provider, resolved } of sources) {
    sheet.byProvider[provider.id] = { [PRESET_FORMAT_KEY]: PRESET_FORMAT };
    for (const field of carriableFields(provider)) {
      const raw = resolved[field.key];
      const value = isEmptyVal(field.type, raw) ? undefined : raw;
      const k = slotKey(provider.id, field.key);
      const held = slots.get(k);
      if (!held) {
        slots.set(k, { field, provider, value, agreed: true });
      } else if (held.value === undefined) {
        held.value = value;
      } else if (value !== undefined && !valuesEqual(field.type, held.value, value)) {
        held.agreed = false;
      }
    }
  }

  const conflicts: string[] = [];
  const own = (s: Slot, value: unknown) => putBagValue(sheet.byProvider, s.provider.id, s.field.key, value);
  // A cross-provider key's slots, and whether any provider conflicted on it.
  const crossProvider = new Map<string, { slots: Slot[]; conflicted: boolean }>();
  for (const s of slots.values()) {
    const group =
      fieldScope(s.field) === "all" ? (crossProvider.get(s.field.key) ?? { slots: [], conflicted: false }) : null;
    if (group) {
      crossProvider.set(s.field.key, group);
    }
    if (!s.agreed) {
      const name = `${s.field.label} (${s.provider.displayName})`;
      if (!conflicts.includes(name)) {
        conflicts.push(name);
      }
      if (group) {
        group.conflicted = true;
      }
    } else if (group) {
      group.slots.push(s);
    } else if (s.value !== undefined) {
      own(s, s.value);
    }
  }
  for (const [key, { slots: agreed, conflicted }] of crossProvider) {
    const first = agreed[0];
    if (!first || agreed.every((s) => s.value === undefined)) {
      continue;
    }
    const alike = agreed.every((s) => s.value !== undefined && valuesEqual(s.field.type, s.value, first.value));
    if (!conflicted && alike) {
      sheet.shared[key] = first.value;
    } else {
      for (const s of agreed) {
        own(s, s.value ?? "");
      }
    }
  }
  return { sheet, conflicts };
}

/** Does this sheet hold anything the user actually put there?
 *
 * An untouched form still collects a sheet: a required enum always resolves to its
 * descriptor default, and prefill seeds every declared default (made for kids: false,
 * auto-stop: true) into the channel. Persisting such a sheet spends one of a small number
 * of preset slots on a row that reads "Untitled" and states no intent.
 *
 * Read per field rather than by comparing whole sheets: a held value at its resting value
 * (isAtRestValue) is one no user choice is needed to explain, and collectPreset has
 * already dropped every unset value, so any other held value is one the user stated --
 * including a stated empty tag list, which is an intent ("send none") rather than the
 * absence of one. That also covers the plainly-empty sheet: nothing held, no intent.
 *
 * The price: a sheet stating ONLY resting values is never saved, even when the user chose
 * them on purpose (private, not made for kids, and nothing else) -- the two cannot be told
 * apart here, and a preset of defaults re-applies nothing the defaults would not.
 *
 * This gates PERSISTENCE only. A sheet still carries its defaults, so applying one sets
 * them -- dropping them from the payload would make the preset fail to set a privacy it
 * was expected to set. */
export function carriesIntent(sheet: PresetSheet, sources: PresetSource[]): boolean {
  for (const { provider } of sources) {
    for (const field of carriableFields(provider)) {
      const held = presetValueOf(sheet, field, provider.id);
      if (held && !isAtRestValue(field, held.value)) {
        return true;
      }
    }
  }
  return false;
}

// ---- Schedule editor: the three columns a planned entry has -----------------------

/** The per-destination metadata a scheduled entry stores. Structurally the entry
 * editor's own row shape, so a patch built here drops straight into patchMeta. */
export interface SchedulePresetMeta {
  title: string;
  category: string;
  categoryId: string;
  tags: string[];
}

// One entry per column `schedule_destinations` actually has, keyed by the descriptor key
// that fills it. A preset field with no entry here has nowhere to land -- the row cannot
// store it -- and is reported rather than dropped.
//
// Each reader returns null for a payload of the wrong shape rather than coercing it to an
// empty column. isEmptyVal answers per descriptor type and cannot see inside a container:
// `tags: [123]` is a non-empty array and passes it, so a coercing reader would hand back
// [] and BLANK the row's existing tags. The values come off disk (stream_info_presets.json
// is hand-editable, and a foreign version may hold shapes this build never wrote), so the
// shape is an input to check, not an invariant to assume.
const SCHEDULE_COLUMNS: Record<string, (v: unknown) => Partial<SchedulePresetMeta> | null> = {
  title: (v) => (typeof v === "string" ? { title: v } : null),
  // Both halves, never one: providers key on the id and the name is only display, so a
  // name without its id is an apply that silently does nothing. A missing display name is
  // not malformed -- the id is what applies.
  category: (v) => {
    const c = v as { id?: unknown; name?: unknown } | null;
    if (typeof c?.id !== "string") {
      return null;
    }
    return { categoryId: c.id, category: typeof c.name === "string" ? c.name : "" };
  },
  tags: (v) =>
    Array.isArray(v) && v.every((t) => typeof t === "string") ? { tags: [...(v as string[])] } : null,
};

export interface SchedulePatch {
  /** What to hand patchMeta. Empty when the preset states nothing this row can hold. */
  patch: Partial<SchedulePresetMeta>;
  /** What the preset carries for this provider that a scheduled entry has no column for
   * -- a description above all. Named so the editor can say it in words instead of
   * dropping it silently. */
  dropped: string[];
  /** Fields whose stored value was the wrong shape to apply. Kept apart from `dropped`:
   * that one names a field this row cannot hold at all, this one names a field it could
   * have held had the value been sound, and the two want different sentences. */
  malformed: string[];
}

/** This preset resolved into one destination's row.
 *
 * The provider's own value wins here as it does in Go Live, so a sheet that kept a
 * different title per platform fills each destination's row with its own platform's. */
export function schedulePatchFor(preset: StreamInfoPreset, provider: OAuthProvider): SchedulePatch {
  const patch: Partial<SchedulePresetMeta> = {};
  const dropped: string[] = [];
  const malformed: string[] = [];
  for (const { field, value } of presetValuesFor(preset, provider)) {
    // hasOwn, not a bare lookup: a descriptor key that happens to name an Object.prototype
    // member ("toString", "constructor") would otherwise resolve to the inherited function
    // and be called as a column reader.
    const column = Object.hasOwn(SCHEDULE_COLUMNS, field.key) ? SCHEDULE_COLUMNS[field.key] : undefined;
    if (!column) {
      // A resting value (made for kids: off, the default latency) is what every sheet
      // carries without anyone choosing it; naming it would report a loss nobody made.
      if (!isAtRestValue(field, value)) {
        dropped.push(field.label);
      }
      continue;
    }
    // A stated empty list is an instruction this editor cannot carry. A scheduled row's
    // empty tags mean "leave the channel's own alone" -- ScheduledSetup.cpp omits the key
    // for them -- so there is no empty value here that means "send none". Writing one in
    // would erase the tags the row already holds while meaning the opposite of that, so
    // the row is left exactly as it was found. Every other type's empty value never
    // reaches this loop, presetValuesFor having dropped it already.
    //
    // Below the lookup above, so a field this row has no column for is still reported as
    // dropped rather than silently skipped for being empty.
    if (isBlankVal(field.type, value)) {
      continue;
    }
    const applied = column(value);
    if (applied === null) {
      malformed.push(field.label);
      continue;
    }
    Object.assign(patch, applied);
  }
  return { patch, dropped, malformed };
}
