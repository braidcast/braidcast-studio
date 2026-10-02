import { describe, expect, test } from "bun:test";
import { eventTypeLabel, followVerb } from "../src/lib/docks/events/eventWording";

describe("event wording", () => {
  test("a YouTube follow reads subscribed / Subscriber", () => {
    expect(followVerb("youtube")).toBe("subscribed");
    expect(eventTypeLabel("follow", "youtube")).toBe("Subscriber");
  });

  test("Twitch and Kick follows are unchanged", () => {
    for (const p of ["twitch", "kick"] as const) {
      expect(followVerb(p)).toBe("followed");
      expect(eventTypeLabel("follow", p)).toBe("Follow");
    }
  });

  test("other types ignore the platform", () => {
    expect(eventTypeLabel("sub", "youtube")).toBe("Sub");
    expect(eventTypeLabel("mystery", "twitch")).toBe("mystery");
  });
});
