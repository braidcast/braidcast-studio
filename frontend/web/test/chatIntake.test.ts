import { describe, expect, test } from "bun:test";
import type { ChatMessage } from "$lib/api/bridge";
import { ChatIntake, chatKey, spansDestinations } from "$lib/docks/multichat/chatIntake";

function msg(id: string, accountId = "twitch:1", profileUuid?: string): ChatMessage {
  return {
    platform: "twitch",
    accountId,
    profileUuid,
    channelId: "c",
    id,
    ts: 0,
    author: { name: "a", color: "", badges: [] },
    fragments: [],
  } as unknown as ChatMessage;
}

function sink() {
  const shown: string[] = [];
  let hydrated = 0;
  return {
    shown,
    get hydrated() {
      return hydrated;
    },
    enqueue(m: ChatMessage) {
      shown.push(m.id);
    },
    setFeed(list: ChatMessage[]) {
      hydrated++;
      shown.length = 0;
      shown.push(...list.map((m) => m.id));
    },
  };
}

describe("ChatIntake hydrate/live", () => {
  test("live frames that land while chat.list is in flight follow the scrollback", () => {
    const s = sink();
    const intake = new ChatIntake(s);
    intake.live(msg("c"));
    expect(s.shown).toEqual([]); // held until the scrollback lands
    intake.hydrate([msg("a"), msg("b")]);
    expect(s.shown).toEqual(["a", "b", "c"]);
  });

  test("a message in both the snapshot and the live stream shows once", () => {
    const s = sink();
    const intake = new ChatIntake(s);
    intake.live(msg("b")); // arrived live after the host had already ringed it
    intake.live(msg("c"));
    intake.hydrate([msg("a"), msg("b")]);
    expect(s.shown).toEqual(["a", "b", "c"]);
  });

  test("after the seam, live frames pass straight through: the host already deduped them", () => {
    const s = sink();
    const intake = new ChatIntake(s);
    intake.hydrate([msg("a")]);
    intake.live(msg("b"));
    intake.live(msg("c"));
    expect(s.shown).toEqual(["a", "b", "c"]);
  });

  test("a held frame with no id cannot be matched, so it is kept", () => {
    const s = sink();
    const intake = new ChatIntake(s);
    intake.live(msg(""));
    intake.hydrate([msg("")]);
    expect(s.shown).toEqual(["", ""]);
  });

  test("the same platform id on two destinations is two messages", () => {
    const s = sink();
    const intake = new ChatIntake(s);
    intake.live(msg("x", "youtube:2", "p2"));
    intake.live(msg("x", "twitch:1"));
    intake.hydrate([msg("x", "youtube:2", "p1")]);
    expect(s.shown).toEqual(["x", "x", "x"]);
  });

  test("a failed chat.list still releases the held frames", () => {
    const s = sink();
    const intake = new ChatIntake(s);
    intake.live(msg("a"));
    intake.hydrate([]);
    expect(s.shown).toEqual(["a"]);
  });

  test("only the first hydrate counts", () => {
    const s = sink();
    const intake = new ChatIntake(s);
    intake.hydrate([msg("a")]);
    intake.hydrate([msg("z")]);
    expect(s.hydrated).toBe(1);
    expect(s.shown).toEqual(["a"]);
  });

  test("rows from two destinations span destinations, whatever is armed now", () => {
    expect(spansDestinations([])).toBe(false);
    expect(spansDestinations([msg("a"), msg("b")])).toBe(false);
    expect(spansDestinations([msg("a", "youtube:2", "p1"), msg("b", "youtube:2", "p2")])).toBe(true);
    expect(spansDestinations([msg("a"), msg("b", "youtube:2")])).toBe(true);
  });

  test("the key is destination plus id, spelled like the host's", () => {
    expect(chatKey(msg("m", "twitch:1"))).toBe("twitch:1:m");
    expect(chatKey(msg("m", "youtube:2", "p1"))).toBe("youtube:2@p1:m");
  });
});
