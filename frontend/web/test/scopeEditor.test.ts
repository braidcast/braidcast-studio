import { describe, expect, test } from "bun:test";
import type { OverlayField } from "../src/lib/api/bridge";
import { withOverride, withScopedOverride } from "../src/lib/overlays/fieldTypes";
import { sampleFor } from "../src/lib/overlays/scopes/replaySample";
import {
  fieldInScope,
  layerOf,
  newVariation,
  newVariationId,
  withLayer,
  withoutVariation,
  withVariationAt,
  withVariationMeta,
  type ScopeLayers,
} from "../src/lib/overlays/scopes/scopeEdit";
import { normalizeScopes, resolveAlertSettings, type AlertScopes } from "../src/overlay/alertScopes";

const EVENTS = normalizeScopes(
  await Bun.file(new URL("../public/overlay/default-alertbox/scopes.json", import.meta.url)).json(),
).events;
const cheer = EVENTS.find((e) => e.key === "cheer")!;
const sub = EVENTS.find((e) => e.key === "sub")!;
const follow = EVENTS.find((e) => e.key === "follow")!;

const layers = (over: Partial<ScopeLayers> = {}): ScopeLayers => ({ settings: {}, overrides: {}, variations: [], ...over });
const scopes = (l: ScopeLayers): AlertScopes => ({ overrides: l.overrides, variations: l.variations, events: EVENTS });

describe("scoped overrides", () => {
  test("writing the inherited value back removes the key, in any layer", () => {
    expect(withScopedOverride({ a: 1 }, "a", 2, 2)).toEqual({});
    expect(withScopedOverride({}, "a", { preset: "zoom-pop" }, { preset: "fade" })).toEqual({ a: { preset: "zoom-pop" } });
    expect(withScopedOverride({}, "a", { preset: "fade" }, { preset: "fade" })).toEqual({});
  });

  test("withOverride is the Defaults case of the same rule", () => {
    const f = { key: "volume", type: "slider", label: "Volume", default: 80 } as OverlayField;
    expect(withOverride({ volume: 50 }, f, 80)).toEqual({});
    expect(withOverride({}, f, 50)).toEqual({ volume: 50 });
  });

  test("an edit lands in the scope's own map, and an emptied event override is dropped", () => {
    let l = layers();
    l = withLayer(l, "cheer", EVENTS, { sound: "library:coin-01" });
    expect(l.overrides).toEqual({ cheer: { sound: "library:coin-01" } });
    expect(layerOf(l, "cheer", EVENTS)).toEqual({ sound: "library:coin-01" });
    l = withLayer(l, "cheer", EVENTS, {});
    expect(l.overrides).toEqual({});
    l = withLayer(l, "default", EVENTS, { volume: 40 });
    expect(l.settings).toEqual({ volume: 40 });
    expect(withLayer(l, "nonsense", EVENTS, { x: 1 })).toBe(l);
  });

  test("a variation's edit lands in its own settings", () => {
    const v = newVariation(cheer, [], () => 0.5)!;
    let l = layers({ variations: [v] });
    l = withLayer(l, v.id, EVENTS, { media: "assets/x.webm" });
    expect(l.variations[0].settings).toEqual({ media: "assets/x.webm" });
    expect(layerOf(l, v.id, EVENTS)).toEqual({ media: "assets/x.webm" });
  });

  test("widget-scope fields are editable in Defaults only", () => {
    const burst = { key: "burstWindow", type: "slider", label: "", default: 2, scope: "widget" } as OverlayField;
    const sound = { key: "sound", type: "sound", label: "", default: "", scope: "alert" } as OverlayField;
    expect(fieldInScope(burst, "default")).toBe(true);
    expect(fieldInScope(burst, "cheer")).toBe(false);
    expect(fieldInScope(sound, "cheer")).toBe(true);
  });
});

describe("variations", () => {
  test("ids are v_ plus four hex digits and never collide", () => {
    const seq = [0.5, 0.5, 0.25];
    const id = newVariationId(["v_8000"], () => seq.shift()!);
    expect(id).toBe("v_4000");
    expect(newVariationId([], () => 0)).toMatch(/^v_[0-9a-f]{4}$/);
  });

  test("a new variation tests its event's first condition; an event with none gets none", () => {
    expect(newVariation(cheer, [])?.when).toEqual({ field: "amount", op: ">=", value: 1000 });
    expect(newVariation(sub, [])?.when).toEqual({ field: "tier", op: ">=", value: 2 });
    expect(newVariation(follow, [])?.when).toEqual({ field: "platform", op: "==", value: "youtube" });
    expect(newVariation({ ...follow, conditions: [] }, [])).toBeNull();
    const superchat = EVENTS.find((e) => e.key === "superchat")!;
    expect(newVariation(superchat, [])?.when).toEqual({ field: "amount", op: ">=", value: 1000, currency: "USD" });
  });

  test("delete then undo puts a variation back where it was", () => {
    const a = newVariation(cheer, [], () => 0.1)!;
    const b = newVariation(cheer, [a.id], () => 0.2)!;
    const c = newVariation(cheer, [a.id, b.id], () => 0.3)!;
    const l = layers({ variations: [a, b, c] });
    const cut = withoutVariation(l, b.id);
    expect(cut.layers.variations.map((v) => v.id)).toEqual([a.id, c.id]);
    expect(withVariationAt(cut.layers, cut.removed!, cut.index).variations.map((v) => v.id)).toEqual([a.id, b.id, c.id]);
    expect(withoutVariation(l, "v_none").removed).toBeNull();
  });

  test("a blank label is removed rather than stored", () => {
    const v = { ...newVariation(cheer, [])!, label: "Big" };
    const l = withVariationMeta(layers({ variations: [v] }), v.id, { label: "  " });
    expect(Object.hasOwn(l.variations[0], "label")).toBe(false);
    const m = withVariationMeta(l, v.id, { when: { field: "amount", op: "==", value: 5 } });
    expect(m.variations[0].when).toEqual({ field: "amount", op: "==", value: 5 });
  });
});

describe("replay samples resolve to the scope they were asked for", () => {
  const big = { id: "v_big", event: "cheer", when: { field: "amount", op: ">=", value: 1000 }, settings: { sound: "x" } };
  const gift = { id: "v_gift", event: "sub", when: { field: "gift", op: "==", value: true }, settings: { sound: "y" } };
  const tier3 = { id: "v_t3", event: "sub", when: { field: "tier", op: ">=", value: 3 }, settings: { sound: "z" } };
  const yt = { id: "v_yt", event: "raid", when: { field: "platform", op: "==", value: "twitch" }, settings: { sound: "p" } };
  const sc = {
    id: "v_sc",
    event: "superchat",
    when: { field: "amount", op: ">=", value: 2000, currency: "INR" },
    settings: { sound: "s" },
  };
  const l = layers({ overrides: { follow: { sound: "f" } }, variations: [big, gift, tier3, yt, sc] });
  const s = scopes(l);
  const resolved = (scope: string) => {
    const sample = sampleFor(s, scope)!;
    return resolveAlertSettings(
      {},
      s,
      { id: "t", type: sample.type as never, ts: 0, actorName: "", ...sample.overrides, platform: sample.overrides.platform as never },
    ).scope;
  };

  test.each(["default", "cheer", "raid", "v_big", "v_gift", "v_t3", "v_yt", "v_sc"])("%s", (scope) => {
    expect(resolved(scope)).toBe(scope === "cheer" || scope === "raid" ? "default" : scope);
  });

  test("a platform variation replays on its platform; its event replays on another", () => {
    expect(sampleFor(s, "v_yt")?.overrides.platform).toBe("twitch");
    expect(sampleFor(s, "raid")?.overrides.platform).not.toBe("twitch");
  });

  test("a money variation replays in its own currency", () => {
    expect(sampleFor(s, "v_sc")?.overrides).toMatchObject({ amount: 2000, currency: "INR" });
  });

  test("a variation replays at its threshold, a gift variation as a gift sub", () => {
    expect(sampleFor(s, "v_big")).toEqual({
      type: "cheer",
      overrides: { amount: 1000, tier: "Tier 1", months: 1, count: 1, platform: "twitch", currency: "USD" },
    });
    expect(sampleFor(s, "v_gift")?.type).toBe("subgift");
    expect(sampleFor(s, "v_t3")?.overrides.tier).toBe("Tier 3");
  });

  test("Defaults skips an event that has its own override", () => {
    expect(sampleFor(s, "default")?.type).not.toBe("follow");
  });

  test("an unknown scope has no sample", () => {
    expect(sampleFor(s, "nope")).toBeNull();
  });
});
