import { describe, expect, test } from "bun:test";
import type { NormalizedEvent } from "../src/lib/api/bridge";
import { eventEmoji, eventSummary } from "../src/overlay/eventSummary";

const fmt = { amount: () => "≈₹4,185 ($50.00)", amountText: () => "1,000 bits" };
const ev = (type: string, extra: Partial<NormalizedEvent> = {}) =>
  ({ id: "1", platform: "twitch", type, ts: 0, actorName: "Ann", ...extra }) as NormalizedEvent;

describe("overlay event wording", () => {
  test("money reads through formatAmount, so overlays carry the streamer's currency", () => {
    expect(eventSummary(ev("superchat", { platform: "youtube", amount: 5000, currency: "USD" }), fmt)).toBe(
      "Super Chat ≈₹4,185 ($50.00)",
    );
    expect(eventSummary(ev("cheer", { amount: 1000 }), fmt)).toBe("cheered 1,000 bits");
  });

  test("a YouTube follow is a subscribe; an unknown type reads as itself", () => {
    expect(eventSummary(ev("follow", { platform: "youtube" }), fmt)).toBe("subscribed");
    expect(eventSummary(ev("follow"), fmt)).toBe("followed");
    expect(eventSummary(ev("hypetrain"), fmt)).toBe("hypetrain");
    expect(eventEmoji("subgift")).toBe("🎁");
    expect(eventEmoji("toString")).toBe("");
  });
});
