import { beforeEach, describe, expect, test } from "bun:test";
import type { VoiceState } from "$lib/api/bridge";
import { VOICE_STATE_EMPTY } from "$lib/stores/voiceStore.svelte";
import { resetKeptDraftForTests, takeKeptDraft } from "$lib/voice/keptDraft";

const to = { accountId: "twitch:1", profileUuid: "p1", replyTo: "bob" };

function kept(id: number, text = "@bob a long reply", overrides: Partial<VoiceState> = {}): VoiceState {
  return { ...VOICE_STATE_EMPTY, state: "idle", keptDraft: text, keptTo: to, keptId: id, ...overrides };
}
const cleared: VoiceState = { ...VOICE_STATE_EMPTY, state: "listening" };

beforeEach(() => resetKeptDraftForTests());

describe("takeKeptDraft", () => {
  test("hands the text and where it was going to an empty composer", () => {
    expect(takeKeptDraft(kept(1), "")).toEqual({ text: "@bob a long reply", to });
  });

  test("once across every composer: a second dock, or the same one remounted, gets nothing", () => {
    expect(takeKeptDraft(kept(1), "")).not.toBeNull();
    expect(takeKeptDraft(kept(1), "")).toBeNull(); // the other dock, same emit
    // A remount sees the store's placeholder before the host answers, then the draft
    // still kept: neither hands it over again.
    expect(takeKeptDraft(VOICE_STATE_EMPTY, "")).toBeNull();
    expect(takeKeptDraft(kept(1), "")).toBeNull();
  });

  test("a composer the user is typing in is left alone, and another empty one takes it", () => {
    expect(takeKeptDraft(kept(1), "typing")).toBeNull();
    expect(takeKeptDraft(kept(1), "")).not.toBeNull();
  });

  test("a new kept draft with the same words is a new one", () => {
    expect(takeKeptDraft(kept(1, "same"), "")).not.toBeNull();
    expect(takeKeptDraft(kept(2, "same"), "")).not.toBeNull();
  });

  test("after the host clears it, a new listener's first kept draft is taken again", () => {
    expect(takeKeptDraft(kept(1), "")).not.toBeNull();
    expect(takeKeptDraft(cleared, "")).toBeNull();
    expect(takeKeptDraft(kept(1), "")).not.toBeNull();
  });

  test("nothing without one", () => {
    expect(takeKeptDraft(cleared, "")).toBeNull();
    expect(takeKeptDraft(kept(0, ""), "")).toBeNull();
  });
});
