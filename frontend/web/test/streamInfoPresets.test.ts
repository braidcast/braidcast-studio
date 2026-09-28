import { describe, expect, test } from "bun:test";
import type { OAuthProvider, OAuthProviderField, StreamInfoPreset } from "$lib/api/bridge";
import { inheritLayers, isEmptyVal, resolveRequiredEnum, slotKey } from "$lib/dialogs/golive/fieldValue";
import {
  PRESET_FORMAT,
  PRESET_FORMAT_KEY,
  applyPresetTo,
  carriesIntent,
  collectPreset,
  presetValuesFor,
  presetDecided,
  schedulePatchFor,
  uncarriedFields,
  withoutPresetDecided,
  type PresetApplyState,
  type PresetChannel,
  type PresetSource,
} from "$lib/dialogs/streamInfoPresets/applyPreset";

// Descriptors trimmed to the keys the preset rules read, mirroring what the host declares
// (youtube_provider.cpp / twitch_provider.cpp / facebook_provider.cpp capabilityJson, plus
// the `safety` stamp bridge.cpp adds from provider.cpp kFieldRules). The Facebook `page`
// field is hypothetical -- no provider declares a per-destination field today -- and stands
// in for any field that addresses a destination.
function field(key: string, type: string, scope: OAuthProviderField["scope"], extra = {}): OAuthProviderField {
  return { key, label: key[0].toUpperCase() + key.slice(1), type, scope, ...extra };
}
const clearable = { clearable: true };

const youtube = {
  id: "youtube",
  displayName: "YouTube",
  fields: [
    field("title", "text", "all"),
    field("description", "textarea", "all", clearable),
    field("category", "category", "provider"),
    field("tags", "tags", "provider", clearable),
    field("thumbnail", "image", "provider", clearable),
    field("privacy", "enum", "provider", {
      ...clearable,
      safety: true,
      required: true,
      default: "private",
      options: [
        { value: "public", label: "Public" },
        { value: "unlisted", label: "Unlisted" },
        { value: "private", label: "Private" },
      ],
    }),
    field("latency", "enum", "channel", {
      ...clearable,
      required: true,
      default: "normal",
      options: [
        { value: "normal", label: "Normal" },
        { value: "low", label: "Low latency" },
      ],
    }),
    field("dvr", "bool", "channel", clearable),
    field("madeForKids", "bool", "channel", { ...clearable, safety: true, default: false }),
    field("autoStop", "bool", "channel", { ...clearable, default: true }),
  ],
} as unknown as OAuthProvider;

const facebook = {
  id: "facebook",
  displayName: "Facebook",
  fields: [
    field("title", "text", "all"),
    field("description", "textarea", "all", clearable),
    field("privacy", "enum", "channel", {
      safety: true,
      required: true,
      default: "public",
      options: [
        { value: "public", label: "Public" },
        { value: "unpublished", label: "Unpublished" },
      ],
    }),
    field("category", "category", "provider", clearable),
    field("page", "text", "provider", { perDestination: true }),
  ],
} as unknown as OAuthProvider;

const twitch = {
  id: "twitch",
  displayName: "Twitch",
  fields: [
    field("title", "text", "all"),
    field("category", "category", "provider"),
    field("tags", "tags", "provider", clearable),
    field("language", "enum", "channel", {
      options: [
        { value: "en", label: "English" },
        { value: "de", label: "German" },
      ],
    }),
    field("contentLabels", "labelset", "channel", clearable),
    field("brandedContent", "bool", "channel", clearable),
  ],
} as unknown as OAuthProvider;

const V = { [PRESET_FORMAT_KEY]: PRESET_FORMAT };

function preset(shared: Record<string, unknown>, byProvider: Record<string, Record<string, unknown>> = {}) {
  return { id: "p", name: "", createdAtMs: 0, lastUsedAtMs: 0, shared, byProvider } as StreamInfoPreset;
}

// The modal's four stores (GoLiveModal.svelte), and its effectiveFields precedence read back:
// a stream override when its switch is on, then the channel's own value, then the nearest
// inherit bucket, then a required enum's default.
function emptyState(): PresetApplyState {
  return { layerValues: {}, channelValues: {}, streamOverrides: {}, streamOverrideOn: {} };
}

interface Ch {
  provider: OAuthProvider;
  accountId: string;
  profileUuids: string[];
  armed: boolean;
}
const yt: Ch = { provider: youtube, accountId: "youtube:1", profileUuids: ["s1"], armed: true };
const yt2: Ch = { provider: youtube, accountId: "youtube:2", profileUuids: ["s4"], armed: false };
const fb: Ch = { provider: facebook, accountId: "facebook:1", profileUuids: ["s2", "s3"], armed: true };
const tw: Ch = { provider: twitch, accountId: "twitch:1", profileUuids: ["s5"], armed: true };
const disarmed = (c: Ch): Ch => ({ ...c, armed: false });

function resolve(state: PresetApplyState, c: Ch, uuid?: string): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const f of c.provider.fields) {
    const stream = uuid && state.streamOverrideOn[uuid] ? state.streamOverrides[uuid]?.[f.key] : undefined;
    const own = state.channelValues[c.accountId]?.[f.key];
    const bucket = inheritLayers(f, c.provider.id)[0];
    const inherited = bucket ? state.layerValues[bucket]?.[f.key] : undefined;
    const value = [stream, own, inherited].find((v) => !isEmptyVal(f.type, v));
    if (value !== undefined) {
      out[f.key] = value;
    } else if (resolveRequiredEnum(f, "") !== "") {
      out[f.key] = resolveRequiredEnum(f, "");
    }
  }
  return out;
}

function hasOverrides(p: OAuthProvider, bag: Record<string, unknown>): boolean {
  return Object.entries(bag).some(([k, v]) => {
    const f = p.fields.find((fd) => fd.key === k);
    return f !== undefined && !f.perDestination && !isEmptyVal(f.type, v);
  });
}

// What the modal hands applyPresetTo: every channel, with its values resolved before the load.
function load(state: PresetApplyState, p: StreamInfoPreset, chans: Ch[]) {
  const channels: PresetChannel[] = chans.map((c) => ({ ...c, resolved: resolve(state, c) }));
  return applyPresetTo(state, p, channels, [youtube, facebook, twitch], hasOverrides);
}

describe("presetValuesFor", () => {
  test("a preset saved before providers could disagree reads exactly as it did", () => {
    const old = preset({ title: "T", description: "D" }, { youtube: { privacy: "public", tags: [] } });
    const read = Object.fromEntries(presetValuesFor(old, youtube).map((v) => [v.field.key, v.value]));
    expect(read).toEqual({ title: "T", description: "D", tags: [], privacy: "public" });
  });

  test("a provider's own value outranks shared, and shared never feeds a provider-scoped field", () => {
    const p = preset({ title: "Shared", category: { id: "9", name: "x" } }, { twitch: { title: "Own", ...V } });
    const tw = presetValuesFor(p, twitch);
    expect(tw.map((v) => [v.field.key, v.value, v.own])).toEqual([["title", "Own", true]]);
    const yt = presetValuesFor(p, youtube);
    expect(yt.map((v) => [v.field.key, v.value, v.own])).toEqual([["title", "Shared", false]]);
  });
});

describe("applyPresetTo: what a preset's silence does", () => {
  // The owner's report: an older sheet saved with no description, loaded after one that had
  // one, changed the title and the thumbnail and left the other broadcast's description.
  test("a preset with no description clears the description it does not hold", () => {
    const state = emptyState();
    state.layerValues = { all: { title: "New", description: "New words" } };
    state.channelValues = { [yt.accountId]: { description: "channel words" } };
    const old = preset({ title: "Old" }, { youtube: { thumbnail: "C:/old.png", privacy: "public" } });
    const { resets } = load(state, old, [yt]);
    const after = resolve(state, yt);
    expect(after).toMatchObject({ title: "Old", thumbnail: "C:/old.png", privacy: "public" });
    expect(after.description).toBeUndefined();
    expect(resets).toEqual(["Description"]);
  });

  test("an older sheet resets no channel-scoped field, safety fields above all", () => {
    const state = emptyState();
    state.channelValues = {
      [yt.accountId]: { latency: "low", madeForKids: true, autoStop: false, dvr: true },
      [fb.accountId]: { privacy: "unpublished" },
    };
    // Written before channel fields travelled: no format marker, so their absence says nothing.
    const old = preset({ title: "T" }, { youtube: { privacy: "public" } });
    const { resets } = load(state, old, [yt, fb]);
    expect(resolve(state, yt)).toMatchObject({ latency: "low", madeForKids: true, autoStop: false, dvr: true });
    expect(resolve(state, fb).privacy).toBe("unpublished");
    expect(resets).toEqual([]);
  });

  test("a current sheet resets its provider's channel fields, but never widens who can watch", () => {
    const state = emptyState();
    state.layerValues = { "provider:youtube": { privacy: "unlisted" } };
    state.channelValues = {
      [yt.accountId]: { latency: "low", madeForKids: true, autoStop: false, dvr: true },
      [fb.accountId]: { privacy: "unpublished" },
    };
    const p = preset({ title: "T" }, { youtube: { ...V }, facebook: { ...V } });
    const { resets } = load(state, p, [yt, fb]);
    expect(resolve(state, yt)).toMatchObject({
      latency: "normal",
      dvr: false,
      autoStop: true,
      madeForKids: true,
      privacy: "unlisted",
    });
    expect(resolve(state, fb).privacy).toBe("unpublished");
    expect(resets).toEqual(["Latency (YouTube)", "Dvr (YouTube)", "AutoStop (YouTube)"]);
  });

  test("a provider the sheet has no bag for keeps its provider-scoped fields", () => {
    const state = emptyState();
    state.layerValues = { "provider:twitch": { tags: ["speedrun"] } };
    load(state, preset({ title: "T" }, { youtube: { ...V } }), [yt, tw]);
    expect(resolve(state, tw).tags).toEqual(["speedrun"]);
  });

  // A reset has to be true on the platform too: judged at Go Live, a field whose reset value
  // would reach the new broadcast as something else is kept.
  test("only a field the platform can clear is reset; the rest keep what they showed, unnamed", () => {
    const state = emptyState();
    state.layerValues = {
      all: { title: "old", description: "old words" },
      "provider:twitch": { tags: ["a"], category: { id: "1", name: "Chess" } },
    };
    state.channelValues = { [tw.accountId]: { language: "de", brandedContent: true, contentLabels: ["Gambling"] } };
    const p = preset({}, { twitch: { ...V }, youtube: { ...V }, facebook: { ...V } });
    const { resets } = load(state, p, [yt, tw, fb]);
    expect(resolve(state, tw)).toEqual({
      title: "old",
      category: { id: "1", name: "Chess" },
      tags: [],
      language: "de",
      contentLabels: [],
      brandedContent: false,
    });
    // Both go-live pushes create a broadcast with no description: YouTube writes it empty,
    // Facebook leaves it out of the new live video.
    expect(resolve(state, yt).description).toBeUndefined();
    expect(resolve(state, fb).description).toBeUndefined();
    expect(resets).toEqual([
      "Description",
      "Tags (Twitch)",
      "ContentLabels (Twitch)",
      "BrandedContent (Twitch)",
    ]);
  });

  test("a sheet no provider of which declares a shared field never resets it", () => {
    const state = emptyState();
    state.layerValues = { all: { description: "keep me" } };
    // Saved from Twitch alone, which has no description to leave out.
    const { resets } = load(state, preset({ title: "T" }, { twitch: { ...V } }), [yt, tw]);
    expect(resolve(state, yt).description).toBe("keep me");
    expect(resets).toEqual([]);
  });

  test("a shared field resets from what the sheet's providers declare, not from the channels open", () => {
    const state = emptyState();
    state.layerValues = { all: { description: "stale" } };
    // Saved in the old format from YouTube and Facebook; Facebook held nothing of its own, so
    // it has no bag, and only a Facebook channel is loading it now.
    const old = preset({ title: "T" }, { youtube: { privacy: "public" } });
    const { resets } = load(state, old, [fb]);
    expect(resolve(state, fb).description).toBeUndefined();
    expect(resets).toEqual(["Description"]);
  });

  test("a current Facebook sheet saved without a category clears the one showing", () => {
    const state = emptyState();
    state.layerValues = { "provider:facebook": { category: { id: "6003", name: "Video games" } } };
    const { resets } = load(state, preset({ title: "T" }, { facebook: { ...V } }), [fb]);
    expect(resolve(state, fb).category).toBeUndefined();
    expect(resets).toEqual(["Category (Facebook)"]);
  });

  test("a current sheet saved without a thumbnail clears the one showing", () => {
    const state = emptyState();
    state.layerValues = { "provider:youtube": { thumbnail: "C:/old.png" } };
    const { resets } = load(state, preset({ title: "T" }, { youtube: { ...V } }), [yt]);
    expect(resolve(state, yt).thumbnail).toBeUndefined();
    expect(resets).toEqual(["Thumbnail (YouTube)"]);
  });

  test("a value already at rest, or showing nothing, is not named", () => {
    const state = emptyState();
    state.channelValues = { [yt.accountId]: { dvr: false, autoStop: true, description: "  ", tags: [] } };
    const { resets } = load(state, preset({ title: "T" }, { youtube: { ...V } }), [yt]);
    expect(resets).toEqual([]);
  });

  test("a stated empty tag list is applied, not treated as silence", () => {
    const state = emptyState();
    state.layerValues = { "provider:youtube": { tags: ["x"] } };
    const { resets } = load(state, preset({ title: "T" }, { youtube: { tags: [] } }), [yt]);
    expect(resolve(state, yt).tags).toEqual([]);
    expect(resets).toEqual([]);
  });
});

describe("applyPresetTo: which channels and layers it reaches", () => {
  test("a channel-scoped value reaches every channel of its provider, armed or not, and no other", () => {
    const state = emptyState();
    state.channelValues = { [yt2.accountId]: { latency: "normal", madeForKids: false } };
    const p = preset({ title: "T" }, { youtube: { ...V, latency: "low", madeForKids: true } });
    load(state, p, [yt, yt2, tw]);
    expect(resolve(state, yt)).toMatchObject({ latency: "low", madeForKids: true, autoStop: true });
    expect(resolve(state, yt2)).toMatchObject({ latency: "low", madeForKids: true, autoStop: true });
    expect(state.channelValues[tw.accountId]?.latency).toBeUndefined();
  });

  test("a disarmed channel ends at the preset's value for its provider, never blank", () => {
    const state = emptyState();
    state.layerValues = { all: { title: "old" } };
    state.channelValues = { [tw.accountId]: { title: "tw old" } };
    const p = preset({}, { youtube: { ...V, title: "YT" }, twitch: { ...V, title: "TW" } });
    const { touchedChannels, resets } = load(state, p, [yt, disarmed(tw)]);
    expect(resolve(state, yt).title).toBe("YT");
    expect(resolve(state, tw).title).toBe("TW");
    expect(touchedChannels).toContain(slotKey(tw.accountId, "title"));
    expect(resets).toEqual([]);
  });

  test("a disarmed channel the preset holds nothing for keeps the title it showed", () => {
    const state = emptyState();
    state.layerValues = { all: { title: "old" } };
    const { touchedChannels, touchedLayers } = load(state, preset({}, { youtube: { ...V, title: "YT" } }), [
      yt,
      disarmed(tw),
    ]);
    expect(resolve(state, yt).title).toBe("YT");
    expect(resolve(state, tw).title).toBe("old");
    // Held, not decided: the bucket under it moved, so only the layer is marked.
    expect(touchedLayers).toContain(slotKey("all", "title"));
    expect(touchedChannels).not.toContain(slotKey(tw.accountId, "title"));
  });

  test("a shared value lands in the bucket and leaves no channel an override of it", () => {
    const state = emptyState();
    state.channelValues = { [yt.accountId]: { title: "own" }, [tw.accountId]: { title: "own" } };
    load(state, preset({ title: "One" }, { youtube: { ...V }, twitch: { ...V } }), [yt, tw]);
    expect(state.layerValues.all.title).toBe("One");
    expect(state.channelValues[yt.accountId].title).toBeUndefined();
    expect(state.channelValues[tw.accountId].title).toBeUndefined();
  });

  test("loading one description after another leaves every channel the second", () => {
    const state = emptyState();
    load(state, preset({ title: "T", description: "A" }, { youtube: { ...V }, facebook: { ...V } }), [yt, fb]);
    expect([resolve(state, yt).description, resolve(state, fb).description]).toEqual(["A", "A"]);
    load(state, preset({ title: "T", description: "B" }, { youtube: { ...V }, facebook: { ...V } }), [yt, fb]);
    expect([resolve(state, yt).description, resolve(state, fb).description]).toEqual(["B", "B"]);
    // Per provider, then shared again: neither provider keeps its own.
    load(state, preset({ title: "T" }, { youtube: { ...V, description: "Y" }, facebook: { ...V, description: "" } }), [
      yt,
      fb,
    ]);
    expect([resolve(state, yt).description, resolve(state, fb).description]).toEqual(["Y", undefined]);
    load(state, preset({ title: "T", description: "B" }, { youtube: { ...V }, facebook: { ...V } }), [yt, fb]);
    expect([resolve(state, yt).description, resolve(state, fb).description]).toEqual(["B", "B"]);
  });

  test("loading a shared-title preset over a per-provider one leaves no provider its old title", () => {
    const state = emptyState();
    load(state, preset({}, { youtube: { ...V, title: "YT" }, twitch: { ...V, title: "TW" } }), [yt, tw]);
    load(state, preset({ title: "One" }, { youtube: { ...V }, twitch: { ...V } }), [yt, tw]);
    expect([resolve(state, yt).title, resolve(state, tw).title]).toEqual(["One", "One"]);
  });

  test("a bucket no channel's outcome moves is not written", () => {
    const state = emptyState();
    state.layerValues = { "provider:twitch": { category: { id: "1", name: "Chess" } } };
    const { touchedLayers } = load(state, preset({ title: "T" }), [tw]);
    expect(touchedLayers).not.toContain(slotKey("provider:twitch", "category"));
    expect(state.layerValues["provider:twitch"].category).toEqual({ id: "1", name: "Chess" });
  });
});

describe("applyPresetTo: stream overrides", () => {
  test("a decided key leaves the streams; a held one stays", () => {
    const state = emptyState();
    state.streamOverrides = { s5: { title: "stream title", language: "de" } };
    state.streamOverrideOn = { s5: true };
    load(state, preset({ title: "T" }, { twitch: { ...V } }), [tw]);
    expect(state.streamOverrides.s5).toEqual({ language: "de" });
    expect(state.streamOverrideOn.s5).toBe(true);
    expect(resolve(state, tw, "s5")).toMatchObject({ title: "T", language: "de" });
  });

  test("the switch goes off only for a stream this load emptied", () => {
    const state = emptyState();
    state.streamOverrides = { s2: { title: "x" }, s3: {} };
    state.streamOverrideOn = { s2: true, s3: true };
    load(state, preset({ title: "T" }), [fb]);
    expect(state.streamOverrideOn.s2).toBe(false);
    // Already on over nothing before the load: the user's own state, left alone.
    expect(state.streamOverrideOn.s3).toBe(true);
  });

  test("a stream holding only its address keeps it", () => {
    const state = emptyState();
    state.streamOverrides = { s2: { page: "123", title: "x" } };
    state.streamOverrideOn = { s2: true };
    load(state, preset({ title: "T" }), [fb]);
    expect(state.streamOverrides.s2).toEqual({ page: "123" });
    expect(state.streamOverrideOn.s2).toBe(false);
  });
});

describe("prefill after a load", () => {
  test("a remembered stream bag loses only the keys the load decided for its channel", () => {
    const state = emptyState();
    const { touchedChannels } = load(state, preset({ title: "T" }, { twitch: { ...V } }), [tw]);
    const decided = new Set(touchedChannels);
    const remembered = { title: "remembered", language: "de", brandedContent: true };
    // Title was set and brandedContent reset; language cannot be cleared, so it was held.
    expect(withoutPresetDecided(remembered, tw.accountId, decided)).toEqual({ language: "de" });
    expect(withoutPresetDecided(undefined, tw.accountId, decided)).toEqual({});
  });

  test("a decided channel key is closed to the refill; a held one and another channel's are not", () => {
    const state = emptyState();
    const { touchedChannels } = load(state, preset({ title: "T" }, { youtube: { ...V } }), [yt, yt2]);
    const decided = new Set(touchedChannels);
    expect(presetDecided(decided, yt.accountId, "latency")).toBe(true);
    expect(presetDecided(decided, yt2.accountId, "latency")).toBe(true);
    expect(presetDecided(decided, yt.accountId, "madeForKids")).toBe(false);
    expect(presetDecided(decided, tw.accountId, "latency")).toBe(false);
  });
});

describe("collectPreset", () => {
  test("destinations that agree share one value, and every armed provider's bag is marked", () => {
    const { sheet, conflicts } = collectPreset([
      { provider: youtube, resolved: { title: "Same" } },
      { provider: twitch, resolved: { title: "Same" } },
    ]);
    expect(sheet).toEqual({ shared: { title: "Same" }, byProvider: { youtube: { ...V }, twitch: { ...V } } });
    expect(conflicts).toEqual([]);
  });

  test("providers that disagree each keep their own value instead of losing it", () => {
    const { sheet, conflicts } = collectPreset([
      { provider: youtube, resolved: { title: "YT", description: "D" } },
      { provider: facebook, resolved: { title: "TW", description: "D" } },
    ]);
    expect(sheet).toEqual({
      shared: { description: "D" },
      byProvider: { youtube: { ...V, title: "YT" }, facebook: { ...V, title: "TW" } },
    });
    expect(conflicts).toEqual([]);
  });

  test("a provider holding no value for a shared field saves its own empty, not the other's", () => {
    const { sheet } = collectPreset([
      { provider: youtube, resolved: { title: "T", description: "D" } },
      { provider: facebook, resolved: { title: "T" } },
    ]);
    expect(sheet).toEqual({
      shared: { title: "T" },
      byProvider: { youtube: { ...V, description: "D" }, facebook: { ...V, description: "" } },
    });
  });

  test("two channels of one provider disagreeing is the one conflict left", () => {
    const { sheet, conflicts } = collectPreset([
      { provider: youtube, resolved: { title: "A", latency: "low" } },
      { provider: youtube, resolved: { title: "B", latency: "low" } },
      { provider: twitch, resolved: { title: "A" } },
    ]);
    expect(conflicts).toEqual(["Title (YouTube)"]);
    // Sharing Twitch's "A" would hand YouTube a title only one of its channels held.
    expect(sheet).toEqual({
      shared: {},
      byProvider: { youtube: { ...V, latency: "low" }, twitch: { ...V, title: "A" } },
    });
  });

  test("channel-scoped fields are collected per provider", () => {
    const { sheet } = collectPreset([
      { provider: youtube, resolved: { title: "T", latency: "low", madeForKids: false, autoStop: true } },
      { provider: twitch, resolved: { title: "T", language: "en" } },
    ]);
    expect(sheet.byProvider).toEqual({
      youtube: { ...V, latency: "low", madeForKids: false, autoStop: true },
      twitch: { ...V, language: "en" },
    });
  });
});

describe("save and apply round trip", () => {
  function saveAndLoad(sources: PresetSource[], state: PresetApplyState, chans: Ch[]) {
    const { sheet } = collectPreset(sources);
    return load(state, preset(sheet.shared, sheet.byProvider), chans);
  }

  test("titles that differ per provider come back per provider, and nothing leaks to a third", () => {
    const state = emptyState();
    state.channelValues = { [fb.accountId]: { title: "stale" }, [tw.accountId]: { title: "older" } };
    saveAndLoad(
      [
        { provider: youtube, resolved: { title: "YT title", description: "desc", privacy: "public" } },
        { provider: twitch, resolved: { title: "TW title", language: "en" } },
      ],
      state,
      // Facebook is armed now but was not at save time; its title cannot be cleared.
      [yt, tw, fb],
    );
    expect(resolve(state, yt)).toMatchObject({ title: "YT title", description: "desc", privacy: "public" });
    expect(resolve(state, tw)).toMatchObject({ title: "TW title", language: "en" });
    expect(resolve(state, fb)).toMatchObject({ title: "stale", description: "desc" });
  });

  test("a description one provider held and another did not goes back only where it was", () => {
    const state = emptyState();
    // Facebook shows another description now; the sheet says it had none.
    state.channelValues = { [fb.accountId]: { description: "other" } };
    saveAndLoad(
      [
        { provider: youtube, resolved: { title: "T", description: "D" } },
        { provider: facebook, resolved: { title: "T" } },
      ],
      state,
      [yt, fb],
    );
    expect(resolve(state, yt).description).toBe("D");
    expect(resolve(state, fb).description).toBeUndefined();
  });

  test("every channel field of a saved channel comes back", () => {
    const resolved = { title: "T", latency: "low", dvr: true, madeForKids: true, autoStop: false };
    const state = emptyState();
    saveAndLoad([{ provider: youtube, resolved }], state, [yt, yt2]);
    expect(resolve(state, yt)).toMatchObject(resolved);
    expect(resolve(state, yt2)).toMatchObject(resolved);
  });
});

describe("carriesIntent", () => {
  const src: PresetSource[] = [{ provider: youtube, resolved: {} }];

  test("a sheet of nothing but resting values and the format marker states no intent", () => {
    const resting = { ...V, privacy: "private", latency: "normal", dvr: false, madeForKids: false, autoStop: true };
    expect(carriesIntent({ shared: {}, byProvider: { youtube: resting } }, src)).toBe(false);
  });

  test("a non-default choice or a stated empty tag list does", () => {
    expect(carriesIntent({ shared: {}, byProvider: { youtube: { madeForKids: true } } }, src)).toBe(true);
    expect(carriesIntent({ shared: {}, byProvider: { youtube: { tags: [] } } }, src)).toBe(true);
    expect(carriesIntent({ shared: {}, byProvider: { youtube: { title: "Own" } } }, src)).toBe(true);
  });
});

describe("schedulePatchFor", () => {
  test("names what the row cannot hold, but not a value nobody chose", () => {
    const p = preset(
      { title: "T" },
      { youtube: { ...V, description: "D", madeForKids: false, latency: "normal", dvr: false, autoStop: true } },
    );
    const { patch, dropped } = schedulePatchFor(p, youtube);
    expect(patch).toEqual({ title: "T" });
    expect(dropped).toEqual(["Description"]);
  });

  test("a provider's own title fills that provider's row", () => {
    const p = preset({}, { youtube: { ...V, title: "YT" }, twitch: { ...V, title: "TW" } });
    expect(schedulePatchFor(p, twitch).patch).toEqual({ title: "TW" });
  });
});

describe("uncarriedFields", () => {
  test("names only a field that addresses a destination", () => {
    expect(uncarriedFields([youtube, twitch, facebook])).toEqual([{ providerName: "Facebook", label: "Page" }]);
    expect(uncarriedFields([youtube, twitch])).toEqual([]);
  });
});
