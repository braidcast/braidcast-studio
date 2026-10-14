import { beforeEach, describe, expect, mock, test } from "bun:test";

// The host as the listener behaves (VoiceListener, Confirm/Cancel): an action is acted on
// only when the click names the one pending now; anything else is refused and changes
// nothing.
let pendingId = 0;
const calls: { method: string; params: unknown }[] = [];
mock.module("$lib/api/bridge", () => ({
  obs: {
    call: (method: string, params?: { id?: number }) => {
      calls.push({ method, params });
      const id = params?.id;
      if (method === "voice.confirm" && (id === undefined || id !== pendingId || pendingId === 0)) {
        return Promise.reject(new Error("refused"));
      }
      if (method === "voice.cancel" && id !== undefined && id !== pendingId) {
        return Promise.reject(new Error("refused"));
      }
      if (method === "voice.confirm" || method === "voice.cancel") {
        pendingId = 0;
        return Promise.resolve({});
      }
      return Promise.reject(new Error("unexpected " + method));
    },
    on: () => () => {},
  },
}));

const { cancelVoice, confirmVoice, takeOverDraft } = await import("$lib/voice/voiceActions");

beforeEach(() => {
  calls.length = 0;
  pendingId = 0;
});

describe("voice actions name the action they were made for", () => {
  test("confirm and cancel send the id", async () => {
    pendingId = 7;
    expect(await confirmVoice(7)).toBe(true);
    pendingId = 8;
    expect(await cancelVoice(8)).toBe(true);
    expect(calls).toEqual([
      { method: "voice.confirm", params: { id: 7 } },
      { method: "voice.cancel", params: { id: 8 } },
    ]);
  });

  test("a click for an action that is gone is refused, and the one pending now stays", async () => {
    pendingId = 9; // draft 7 expired; "Stop streaming?" is pending now
    expect(await confirmVoice(7)).toBe(false);
    expect(await cancelVoice(7)).toBe(false);
    expect(pendingId).toBe(9);
  });
});

describe("takeOverDraft (the Edit button)", () => {
  const draft = { id: 4, text: "@bob hi", to: { accountId: "twitch:1", profileUuid: "p1", replyTo: "bob" } };

  test("hands the text and its address over once the host has cancelled the draft", async () => {
    pendingId = 4;
    expect(await takeOverDraft(draft)).toEqual({ text: "@bob hi", to: draft.to });
    expect(calls).toEqual([{ method: "voice.cancel", params: { id: 4 } }]);
  });

  test("hands nothing over when the draft went out first, so it is never sent twice", async () => {
    pendingId = 0; // the countdown sent it as Edit was clicked
    expect(await takeOverDraft(draft)).toBeNull();
  });

  test("hands nothing over, and cancels nothing, when another action is pending by now", async () => {
    pendingId = 5;
    expect(await takeOverDraft(draft)).toBeNull();
    expect(pendingId).toBe(5);
  });
});
