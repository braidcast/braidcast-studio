import type { OAuthProvider, OAuthProviderField, StreamInfoPreset } from "$lib/api/bridge";
import {
  ALL_LAYER,
  inheritLayers,
  isBlankVal,
  isEmptyVal,
  resolveRequiredEnum,
  valuesEqual,
} from "$lib/dialogs/golive/fieldValue";

// What a preset carries, and where each value belongs, held once for both surfaces that
// load one -- the Go Live modal (which writes into its inherit layers) and the schedule
// entry editor (which writes into its flat per-destination rows). Two hand-written copies
// of this rule would be two answers to "does this field belong in a preset", and the
// answer decides whether applying a sheet silently re-addresses a destination.
//
// Membership is `inheritLayers`, never a scope test of our own: it already returns no
// bucket for a per-destination field (Facebook's claimed Page rides in the per-stream bag
// as one) and none for a channel-scoped field (a value one platform's rules forbid on
// another). A preset carries exactly what has a bucket, which is exactly what may travel.

/** One value a preset holds for one declared field, with the layer bucket it came from. */
export interface PresetFieldValue {
  field: OAuthProviderField;
  /** ALL_LAYER or `provider:<id>` -- the bucket that sourced it and the one it writes to. */
  bucket: string;
  value: unknown;
}

function bagFor(preset: StreamInfoPreset, bucket: string, providerId: string): Record<string, unknown> {
  return bucket === ALL_LAYER ? preset.shared : (preset.byProvider[providerId] ?? {});
}

/** The declared fields of `provider` a preset may carry at all, whether or not this one
 * holds a value for them. */
export function carriableFields(provider: OAuthProvider): OAuthProviderField[] {
  return provider.fields.filter((f) => inheritLayers(f, provider.id).length > 0);
}

/** The declared fields of `provider` that no preset can carry -- their value is bound to
 * one channel or addresses one destination, so it stays where it is. */
export function uncarriableFields(provider: OAuthProvider): OAuthProviderField[] {
  return provider.fields.filter((f) => inheritLayers(f, provider.id).length === 0);
}

/** What this preset actually states for `provider`: one entry per declared field the
 * preset holds a value for, skipping every field it says nothing about. Silence is not an
 * instruction to blank a field, so an omitted key produces no entry and every caller
 * leaves that field alone.
 *
 * A stated empty tag list is not silence — isEmptyVal keeps the two apart for that type —
 * so a sheet saved from a channel the user deliberately cleared re-applies the clear
 * rather than quietly leaving the next channel's tags standing. */
export function presetValuesFor(preset: StreamInfoPreset, provider: OAuthProvider): PresetFieldValue[] {
  const out: PresetFieldValue[] = [];
  for (const field of provider.fields) {
    for (const bucket of inheritLayers(field, provider.id)) {
      const value = bagFor(preset, bucket, provider.id)[field.key];
      if (!isEmptyVal(field.type, value)) {
        out.push({ field, bucket, value });
        break;
      }
    }
  }
  return out;
}

/** The label a preset reads by: its own name, else the title it carries (shared first,
 * then any provider's own), else a stand-in -- a row with no words on it is unpickable. */
export function presetLabel(preset: StreamInfoPreset): string {
  const named = preset.name.trim();
  if (named !== "") {
    return named;
  }
  const bags = [preset.shared, ...Object.values(preset.byProvider)];
  for (const bag of bags) {
    const title = bag["title"];
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

/** What applying a preset leaves untouched, across the providers actually in play. The
 * list is computed from the descriptors, so a provider that widens or narrows a field's
 * scope changes this sentence without an edit here. */
export function uncarriedFields(providers: OAuthProvider[]): UncarriedField[] {
  const seen = new Set<string>();
  const out: UncarriedField[] = [];
  for (const p of providers) {
    for (const f of uncarriableFields(p)) {
      const key = p.id + "::" + f.key;
      if (!seen.has(key)) {
        seen.add(key);
        out.push({ providerName: p.displayName, label: f.label });
      }
    }
  }
  return out;
}

// ---- Go Live: one write per key, three layers ------------------------------------

/** A channel a preset is being applied to, reduced to what the rule needs. */
export interface PresetTarget {
  provider: OAuthProvider;
  accountId: string;
  /** Every stream on this channel -- a preset value clears the key's per-stream
   * override as well, or the layer it just set would be outranked and read as inert. */
  profileUuids: string[];
}

/** One key's landing: set it in `bucket`, and clear that same key from the channel bag
 * and from every one of the channel's stream bags. Key-scoped on purpose -- a stream
 * keeps the overrides the preset says nothing about. */
export interface PresetLayerWrite {
  bucket: string;
  key: string;
  value: unknown;
  accountId: string;
  profileUuids: string[];
}

/** Every write applying `preset` to `targets` comes to. Ordering is irrelevant: at most
 * one write exists per (bucket, key), since a field resolves to a single bucket. */
export function presetLayerWrites(preset: StreamInfoPreset, targets: PresetTarget[]): PresetLayerWrite[] {
  const out: PresetLayerWrite[] = [];
  for (const t of targets) {
    for (const { field, bucket, value } of presetValuesFor(preset, t.provider)) {
      out.push({
        bucket,
        key: field.key,
        value,
        accountId: t.accountId,
        profileUuids: t.profileUuids,
      });
    }
  }
  return out;
}

/** One key to clear from one channel's own bag and from its streams' bags. */
export interface PresetClearSite {
  accountId: string;
  profileUuids: string[];
  key: string;
}

/** Where `writes` must additionally be CLEARED from, over a scope wider than the channels
 * the preset was aimed at.
 *
 * A write lands in an inherit bucket, which effectiveFields ranks LAST, so any channel or
 * stream still holding its own value for that key outranks it. Buckets are global -- one
 * cross-provider bucket and one per provider -- so a write made for the armed channels is
 * already visible to every channel that reads the same bucket, and a clear aimed only at
 * the armed ones leaves the hazard standing on the rest: a channel disarmed at apply time
 * keeps the channelValues entry an earlier prefill wrote, and re-arming it before confirm
 * pushes that OLD value while the form shows the preset's. Matching the clear's reach to
 * the write's reach is what closes that.
 *
 * Keyed on the (bucket, key) actually written, so a provider-scoped key written for one
 * platform never clears a same-named key belonging to another platform's bucket. The "::"
 * join is unambiguous only because neither half can contain a colon -- buckets are built
 * from registry provider ids and the keys are descriptor field keys, both developer-owned
 * identifiers, never user text. */
export function presetClearSites(writes: PresetLayerWrite[], scope: PresetTarget[]): PresetClearSite[] {
  const written = new Set(writes.map((w) => w.bucket + "::" + w.key));
  const out: PresetClearSite[] = [];
  for (const t of scope) {
    for (const field of carriableFields(t.provider)) {
      if (written.has(inheritLayers(field, t.provider.id)[0] + "::" + field.key)) {
        out.push({ accountId: t.accountId, profileUuids: t.profileUuids, key: field.key });
      }
    }
  }
  return out;
}

/** One channel's resolved values, as the Go Live modal already computes them for a push. */
export interface PresetSource {
  provider: OAuthProvider;
  /** The channel's effective fields (every layer merged, empties already dropped). */
  resolved: Record<string, unknown>;
}

/** The two bags exactly as the host stores them. */
export interface PresetSheet {
  shared: Record<string, unknown>;
  byProvider: Record<string, Record<string, unknown>>;
}

export interface CollectedPreset {
  sheet: PresetSheet;
  /** Field labels the sheet deliberately does NOT carry, because two channels feeding the
   * same bucket held different values and there is one slot for them. */
  conflicts: string[];
}

/** The sheet to store: the resolved value of every field that has a bucket, sorted into
 * the same two bags applying reads back out.
 *
 * `sources` is one entry per armed ACCOUNT, not per provider, so several of them can feed
 * one bucket -- two accounts of one provider always do, and every cross-provider field
 * does by definition. Each bucket holds one value, so when the contributors disagree there
 * is no answer that is not a guess: keeping either one would save a sheet claiming to be
 * this go-live's stream info while carrying only half of it, and the user cannot see which
 * half. Such a key is left out entirely and named in `conflicts` instead. A key nobody
 * disagrees about -- the ordinary case of one title across every destination -- collects
 * exactly as before.
 *
 * Symmetry with presetValuesFor therefore holds PER KEY: every key that survives is one
 * presetValuesFor will hand back. It does not hold across channels, and deliberately so --
 * a divergence the modal renders as an "overrides shared" chip cannot be stored in a
 * single-valued bag, so it is dropped rather than flattened. */
export function collectPreset(sources: PresetSource[]): CollectedPreset {
  // Keyed by bucket + key, which is the storage slot itself: a provider bucket already
  // carries its provider id, so two accounts of one provider land on the same slot and
  // are compared, exactly like two providers sharing the cross-provider bucket.
  const slots = new Map<
    string,
    { field: OAuthProviderField; bucket: string; providerId: string; value: unknown; agreed: boolean }
  >();
  for (const { provider, resolved } of sources) {
    for (const field of carriableFields(provider)) {
      const value = resolved[field.key];
      if (isEmptyVal(field.type, value)) {
        continue;
      }
      const bucket = inheritLayers(field, provider.id)[0];
      const slotKey = bucket + "::" + field.key;
      const held = slots.get(slotKey);
      if (!held) {
        slots.set(slotKey, { field, bucket, providerId: provider.id, value, agreed: true });
      } else if (!valuesEqual(field.type, held.value, value)) {
        held.agreed = false;
      }
    }
  }

  const sheet: PresetSheet = { shared: {}, byProvider: {} };
  const conflicts: string[] = [];
  for (const slot of slots.values()) {
    if (!slot.agreed) {
      if (!conflicts.includes(slot.field.label)) {
        conflicts.push(slot.field.label);
      }
      continue;
    }
    if (slot.bucket === ALL_LAYER) {
      sheet.shared[slot.field.key] = slot.value;
    } else {
      sheet.byProvider[slot.providerId] = {
        ...(sheet.byProvider[slot.providerId] ?? {}),
        [slot.field.key]: slot.value,
      };
    }
  }
  return { sheet, conflicts };
}

/** Does this sheet hold anything the user actually put there?
 *
 * A required field has no valid empty state, so effectiveFields emits its descriptor
 * default whether or not any layer was ever filled -- which means an untouched form still
 * collects a sheet, one carrying nothing but those defaults. Persisting it spends one of a
 * small number of preset slots on a row that reads "Untitled" and states no intent.
 *
 * Read per field rather than by comparing whole sheets: resolveRequiredEnum(field, "") is
 * the same call effectiveFields makes for an all-empty field, so a held value equal to it
 * is provably one no layer held. It answers "" for everything that is not a required enum,
 * and collectPreset has already dropped every unset value, so any other held value is one
 * the user stated — including a stated empty tag list, which is an intent ("send none")
 * rather than the absence of one. That also covers the plainly-empty sheet: nothing held,
 * nothing to disagree with a default, no intent.
 *
 * This gates PERSISTENCE only. A sheet still carries its defaults, so applying one sets
 * them -- dropping them from the payload would make the preset fail to set a privacy it
 * was expected to set. */
export function carriesIntent(sheet: PresetSheet, sources: PresetSource[]): boolean {
  for (const { provider } of sources) {
    for (const field of carriableFields(provider)) {
      const bucket = inheritLayers(field, provider.id)[0];
      const held =
        bucket === ALL_LAYER ? sheet.shared[field.key] : sheet.byProvider[provider.id]?.[field.key];
      if (held !== undefined && held !== resolveRequiredEnum(field, "")) {
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

/** This preset resolved into one destination's row. */
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
      dropped.push(field.label);
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
