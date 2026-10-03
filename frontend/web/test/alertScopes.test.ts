import { describe, expect, test } from "bun:test";
import type { NormalizedEvent } from "../src/lib/api/bridge";
import {
  conditionLabel,
  conditionProblem,
  inheritedValue,
  normalizeScopes,
  resolveAlertSettings,
  scopeChain,
  type AlertScopes,
  type AlertVariation,
} from "../src/overlay/alertScopes";

const SCOPES_JSON = await Bun.file(new URL("../public/overlay/default-alertbox/scopes.json", import.meta.url)).json();
const EVENTS = normalizeScopes(SCOPES_JSON).events;

const ev = (type: NormalizedEvent["type"], extra: Partial<NormalizedEvent> = {}): NormalizedEvent => ({
  id: "e1",
  platform: "twitch",
  type,
  ts: 0,
  actorName: "viewer",
  ...extra,
});

const variation = (
  id: string,
  event: string,
  field: string,
  op: string,
  value: unknown,
  settings: Record<string, unknown>,
  currency?: string,
): AlertVariation => ({
  id,
  event,
  when: currency === undefined ? { field, op, value } : { field, op, value, currency },
  settings,
});

const scopes = (over: Partial<AlertScopes> = {}): AlertScopes => ({ overrides: {}, variations: [], events: EVENTS, ...over });

const DEFAULTS = { sound: "library:chime-01", volume: 80, message: "", inAnim: { preset: "slide-up" } };

describe("resolveAlertSettings", () => {
  test("a field no scope sets falls through to Defaults", () => {
    const r = resolveAlertSettings(DEFAULTS, scopes({ overrides: { cheer: { volume: 40 } } }), ev("cheer", { amount: 100 }));
    expect(r.settings.sound).toBe("library:chime-01");
    expect(r.settings.volume).toBe(40);
    expect(r.scope).toBe("cheer");
  });

  test("the event override applies, then the winning variation on top of it", () => {
    const s = scopes({
      overrides: { cheer: { sound: "library:coin-01", volume: 50 } },
      variations: [variation("v_a", "cheer", "amount", ">=", 1000, { sound: "library:jingle-01" })],
    });
    const r = resolveAlertSettings(DEFAULTS, s, ev("cheer", { amount: 1500 }));
    expect(r.settings.sound).toBe("library:jingle-01");
    expect(r.settings.volume).toBe(50);
    expect(r.scope).toBe("v_a");
  });

  test("the highest matching threshold wins regardless of list order", () => {
    const s = scopes({
      variations: [
        variation("v_100", "cheer", "amount", ">=", 100, { sound: "a" }),
        variation("v_1000", "cheer", "amount", ">=", 1000, { sound: "b" }),
        variation("v_500", "cheer", "amount", ">=", 500, { sound: "c" }),
      ],
    });
    expect(resolveAlertSettings(DEFAULTS, s, ev("cheer", { amount: 5000 })).scope).toBe("v_1000");
    expect(resolveAlertSettings(DEFAULTS, s, ev("cheer", { amount: 700 })).scope).toBe("v_500");
    expect(resolveAlertSettings(DEFAULTS, s, ev("cheer", { amount: 50 })).scope).toBe("default");
  });

  test("equal thresholds go to list order", () => {
    const s = scopes({
      variations: [
        variation("first", "cheer", "amount", ">=", 100, { sound: "a" }),
        variation("second", "cheer", "amount", ">=", 100, { sound: "b" }),
      ],
    });
    expect(resolveAlertSettings(DEFAULTS, s, ev("cheer", { amount: 100 })).scope).toBe("first");
  });

  test("a condition that cannot be evaluated never matches", () => {
    const s = scopes({
      variations: [
        variation("unknown-field", "cheer", "colour", ">=", 1, { sound: "x" }),
        variation("text-on-numeric", "cheer", "amount", ">=", "lots", { sound: "x" }),
        variation("bad-op", "cheer", "amount", "<", 1, { sound: "x" }),
        variation("gift-gte", "sub", "gift", ">=", true, { sound: "x" }),
      ],
    });
    expect(resolveAlertSettings(DEFAULTS, s, ev("cheer", { amount: 1e9 })).scope).toBe("default");
    expect(resolveAlertSettings(DEFAULTS, s, ev("subgift", { count: 5 })).scope).toBe("default");
  });

  test("sub covers resubs and gift subs; gift, tier, months and count conditions read them", () => {
    const s = scopes({
      variations: [
        variation("gifted", "sub", "gift", "==", true, { sound: "g" }),
        variation("tier3", "sub", "tier", ">=", 3, { sound: "t" }),
        variation("year", "sub", "months", ">=", 12, { sound: "m" }),
        variation("bomb", "sub", "count", ">=", 20, { sound: "c" }),
      ],
    });
    expect(resolveAlertSettings(DEFAULTS, s, ev("subgift", { count: 5, tier: "Tier 1" })).scope).toBe("gifted");
    expect(resolveAlertSettings(DEFAULTS, s, ev("subgift", { count: 50, tier: "Tier 1" })).scope).toBe("bomb");
    expect(resolveAlertSettings(DEFAULTS, s, ev("resub", { tier: "Tier 3", months: 2 })).scope).toBe("tier3");
    expect(resolveAlertSettings(DEFAULTS, s, ev("resub", { tier: "Tier 1", months: 13 })).scope).toBe("year");
    expect(resolveAlertSettings(DEFAULTS, s, ev("sub", { tier: "Tier 1", months: 1 })).scope).toBe("default");
  });

  test("a membership level name matches tier == by name and never matches tier >=", () => {
    const s = scopes({
      variations: [
        variation("gold", "member", "tier", "==", "Gold", { sound: "g" }),
        variation("tier2", "member", "tier", ">=", 2, { sound: "t" }),
      ],
    });
    expect(resolveAlertSettings(DEFAULTS, s, ev("member", { tier: "gold" })).scope).toBe("gold");
    expect(resolveAlertSettings(DEFAULTS, s, ev("member", { tier: "Silver" })).scope).toBe("default");
  });

  test("a tally type with no amount on the wire reads as zero", () => {
    const s = scopes({ variations: [variation("zero", "raid", "amount", "==", 0, { sound: "z" })] });
    expect(resolveAlertSettings(DEFAULTS, s, ev("raid")).scope).toBe("zero");
  });

  test("an empty message falls back to the event's built-in message", () => {
    const r = resolveAlertSettings(DEFAULTS, scopes(), ev("follow"));
    expect(r.settings.message).toBe(EVENTS.find((e) => e.key === "follow")!.message);
    const set = resolveAlertSettings(DEFAULTS, scopes({ overrides: { follow: { message: "hi {name}" } } }), ev("follow"));
    expect(set.settings.message).toBe("hi {name}");
  });

  test("a YouTube follow says subscribed; Twitch and Kick follows say followed", () => {
    expect(resolveAlertSettings(DEFAULTS, scopes(), ev("follow", { platform: "youtube" })).settings.message).toBe(
      "{name} just subscribed!",
    );
    for (const platform of ["twitch", "kick"] as const) {
      expect(resolveAlertSettings(DEFAULTS, scopes(), ev("follow", { platform })).settings.message).toBe(
        "{name} just followed!",
      );
    }
  });

  test("a message any scope sets wins over the platform's built-in one", () => {
    const s = scopes({ overrides: { follow: { message: "hi {name}" } } });
    expect(resolveAlertSettings(DEFAULTS, s, ev("follow", { platform: "youtube" })).settings.message).toBe("hi {name}");
  });

  test("a platform variation matches that platform only", () => {
    const s = scopes({ variations: [variation("yt", "follow", "platform", "==", "youtube", { message: "sub {name}" })] });
    const yt = resolveAlertSettings(DEFAULTS, s, ev("follow", { platform: "youtube" }));
    expect([yt.scope, yt.settings.message]).toEqual(["yt", "sub {name}"]);
    expect(resolveAlertSettings(DEFAULTS, s, ev("follow", { platform: "twitch" })).scope).toBe("default");
  });

  test("a money threshold matches only in its own currency", () => {
    const s = scopes({
      variations: [
        variation("usd20", "superchat", "amount", ">=", 2000, { sound: "u" }, "USD"),
        variation("inr1000", "superchat", "amount", ">=", 100000, { sound: "i" }, "INR"),
      ],
    });
    const sc = (amount: number, currency: string) =>
      resolveAlertSettings(DEFAULTS, s, ev("superchat", { platform: "youtube", amount, currency })).scope;
    expect(sc(2000, "USD")).toBe("usd20");
    expect(sc(2000, "INR")).toBe("default");
    expect(sc(150000, "INR")).toBe("inr1000");
    expect(sc(150000, "JPY")).toBe("default");
  });

  test("a money condition without a currency never matches", () => {
    const s = scopes({ variations: [variation("bare", "superchat", "amount", ">=", 1, { sound: "x" })] });
    expect(resolveAlertSettings(DEFAULTS, s, ev("superchat", { amount: 500, currency: "USD" })).scope).toBe("default");
  });

  test("a type no scope declares resolves to Defaults untouched", () => {
    const r = resolveAlertSettings(DEFAULTS, scopes({ events: [] }), ev("cheer"));
    expect(r).toEqual({ settings: DEFAULTS, scope: "default", eventKey: null });
  });

  test("a variation of another event never applies", () => {
    const s = scopes({ variations: [variation("v", "raid", "amount", ">=", 1, { sound: "r" })] });
    expect(resolveAlertSettings(DEFAULTS, s, ev("cheer", { amount: 10 })).scope).toBe("default");
  });
});

describe("editor helpers", () => {
  const s = scopes({
    overrides: { cheer: { sound: "library:coin-01" } },
    variations: [variation("v_big", "cheer", "amount", ">=", 1000, { volume: 100 })],
  });

  test("scope chains run nearest first", () => {
    expect(scopeChain(s, "default")).toEqual([]);
    expect(scopeChain(s, "cheer")).toEqual(["default"]);
    expect(scopeChain(s, "v_big")).toEqual(["cheer", "default"]);
  });

  test("inherited values name the scope they come from", () => {
    expect(inheritedValue(s, DEFAULTS, "v_big", "sound")).toEqual({ value: "library:coin-01", from: "cheer" });
    expect(inheritedValue(s, DEFAULTS, "v_big", "volume")).toEqual({ value: 80, from: "default" });
    expect(inheritedValue(s, DEFAULTS, "cheer", "message").value).toBe(EVENTS.find((e) => e.key === "cheer")!.message);
  });

  test("a platform variation inherits that platform's built-in message", () => {
    const p = scopes({ variations: [variation("yt", "follow", "platform", "==", "youtube", {})] });
    expect(inheritedValue(p, DEFAULTS, "yt", "message")).toEqual({ value: "{name} just subscribed!", from: "default" });
    expect(inheritedValue(p, DEFAULTS, "follow", "message").value).toBe("{name} just followed!");
  });

  test("condition problems are explained and narrowed to the event's own fields", () => {
    const cheer = EVENTS.find((e) => e.key === "cheer")!;
    expect(conditionProblem({ field: "amount", op: ">=", value: 1000 }, cheer)).toBeNull();
    expect(conditionProblem({ field: "months", op: ">=", value: 3 }, cheer)).toContain("carry no months");
    expect(conditionProblem({ field: "amount", op: ">=", value: "x" }, cheer)).toContain("needs a number");
    const sc = EVENTS.find((e) => e.key === "superchat")!;
    expect(conditionProblem({ field: "amount", op: ">=", value: 500 }, sc)).toContain("currency");
    expect(conditionProblem({ field: "amount", op: ">=", value: 500, currency: "usd" }, sc)).toBeNull();
    const follow = EVENTS.find((e) => e.key === "follow")!;
    expect(conditionProblem({ field: "platform", op: "==", value: "youtube" }, follow)).toBeNull();
    expect(conditionProblem({ field: "platform", op: ">=", value: "youtube" }, follow)).toContain("cannot be compared");
    expect(conditionProblem({ field: "platform", op: "==", value: "youtube" }, sc)).toContain("carry no platform");
  });

  test("condition labels read like the rail shows them", () => {
    const cheer = EVENTS.find((e) => e.key === "cheer")!;
    const sc = EVENTS.find((e) => e.key === "superchat")!;
    expect(conditionLabel(variation("a", "cheer", "amount", ">=", 1000, {}), cheer)).toBe("≥ 1,000 bits");
    expect(conditionLabel(variation("b", "superchat", "amount", ">=", 500, {}), sc)).toBe("≥ 5.00");
    expect(conditionLabel(variation("b2", "superchat", "amount", ">=", 500, {}, "USD"), sc)).toBe(
      `≥ ${new Intl.NumberFormat(undefined, { style: "currency", currency: "USD" }).format(5)}`,
    );
    expect(conditionLabel(variation("e", "follow", "platform", "==", "youtube", {}))).toBe("On YouTube");
    expect(conditionLabel(variation("c", "sub", "tier", ">=", 2, {}))).toBe("≥ Tier 2");
    expect(conditionLabel(variation("d", "sub", "gift", "==", true, {}))).toBe("Gift subs");
  });

  test("normalizeScopes drops malformed entries instead of throwing", () => {
    const n = normalizeScopes({ overrides: { cheer: 3, raid: { volume: 1 } }, variations: [null, { id: 1 }], events: "x" });
    expect(n).toEqual({ overrides: { raid: { volume: 1 } }, variations: [], events: [] });
  });
});
