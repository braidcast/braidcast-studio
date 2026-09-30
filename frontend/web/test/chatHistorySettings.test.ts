import { describe, expect, test } from "bun:test";
import type { ChatHistoryRetention, ChatHistoryState, ChatHistoryStatus } from "$lib/api/bridge";
import {
  RETENTION_OPTIONS,
  historyStatusText,
  isRetention,
  retentionChangeDeletes,
  sessionOnlyHint,
  sessionOnlyPlatforms,
} from "$lib/settings/chatHistory";

const MODES: ChatHistoryRetention[] = ["off", "session", "7d"];

describe("retention tokens", () => {
  test("the select offers exactly the host's three tokens, in increasing retention", () => {
    // Chat::kRetentionTokens in frontend/src/chat/chat_retention.hpp.
    expect(RETENTION_OPTIONS.map((o) => o.value)).toEqual(["off", "session", "7d"]);
  });

  test("anything else is not a retention", () => {
    expect(MODES.every(isRetention)).toBe(true);
    expect(isRetention("7D")).toBe(false);
    expect(isRetention("")).toBe(false);
  });
});

describe("retentionChangeDeletes", () => {
  test("asks exactly for the changes that delete stored chat", () => {
    const asks = MODES.flatMap((from) =>
      MODES.filter((to) => retentionChangeDeletes(from, to)).map((to) => `${from}>${to}`),
    );
    expect(asks).toEqual(["session>off", "7d>off", "7d>session"]);
  });

  test("an unknown stored value counts as keeping the most", () => {
    expect(MODES.filter((to) => retentionChangeDeletes("30d", to))).toEqual(["off", "session"]);
  });
});

describe("session-only platforms", () => {
  test("every chat platform the host does not moderate, named", () => {
    expect(sessionOnlyPlatforms(["twitch", "youtube"])).toEqual(["Kick", "Facebook Live"]);
  });

  test("matches the host's list whatever its case", () => {
    expect(sessionOnlyPlatforms(["Twitch", " YouTube "])).toEqual(["Kick", "Facebook Live"]);
  });

  test("the hint speaks of 7 days, joins the names, and disappears once all are moderated", () => {
    expect(sessionOnlyHint(["twitch", "youtube"])).toStartWith(
      "With 7 days, Kick and Facebook Live chat is still kept for this session only",
    );
    expect(sessionOnlyHint(["twitch", "youtube", "kick", "facebook"])).toBe("");
  });
});

describe("historyStatusText", () => {
  const status = (over: Partial<ChatHistoryStatus>): ChatHistoryStatus => ({
    status: "ok",
    detail: "",
    rows: 0,
    onDisk: false,
    moderatedPlatforms: [],
    ...over,
  });

  test("counts stored messages", () => {
    expect(historyStatusText(status({ rows: 1 }))).toEqual({ text: "1 message stored.", problem: false });
    expect(historyStatusText(status({ rows: 10000 })).text).toBe("10,000 messages stored.");
  });

  test("off says nothing is stored only when no file is left", () => {
    expect(historyStatusText(status({ status: "off" }))).toEqual({ text: "Nothing is stored.", problem: false });
    const left = historyStatusText(status({ status: "off", onDisk: true }));
    expect(left.problem).toBe(true);
    expect(left.text).not.toContain("Nothing is stored");
  });

  test("names a set-aside file only when there is one", () => {
    const name = "chat.db.corrupt-2026-09-29_10-00-00";
    expect(historyStatusText(status({ status: "recovered", detail: name })).text).toContain(name);
    expect(historyStatusText(status({ status: "recovered" })).text).toContain("deleted");
  });

  test("an unknown stored setting is named and flagged", () => {
    const line = historyStatusText(status({ status: "unknown-setting", detail: "7D" }));
    expect(line.problem).toBe(true);
    expect(line.text).toContain('"7D"');
    expect(line.text).toContain("up to 7 days");
    expect(line.text).toContain("new chat is not saved");
  });

  test("names a file left because it could not be read", () => {
    const line = historyStatusText(status({ status: "unreadable", detail: "chat.db", onDisk: true }));
    expect(line.problem).toBe(true);
    expect(line.text).toContain("chat.db");
    expect(line.text).toContain("left in place");
  });

  test("flags every problem state, and a status this build does not know", () => {
    for (const s of ["recovered", "newer-schema", "disabled", "degraded", "unknown-setting", "unreadable"] as const) {
      expect(historyStatusText(status({ status: s, detail: "why" })).problem).toBe(true);
    }
    const unknown = historyStatusText(status({ status: "unknown" as ChatHistoryState }));
    expect(unknown.problem).toBe(true);
    expect(unknown.text.length).toBeGreaterThan(0);
  });
});
