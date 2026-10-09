import { describe, expect, mock, test } from "bun:test";
import type { VoiceState } from "$lib/api/bridge";

let answer: ((s: VoiceState) => void) | null = null;
let reads = 0;
const listeners = new Map<string, (p: VoiceState) => void>();
mock.module("$lib/api/bridge", () => ({
  obs: {
    call: (method: string) => {
      if (method !== "voice.state") {
        throw new Error("unexpected " + method);
      }
      reads++;
      return new Promise<VoiceState>((resolve) => (answer = resolve));
    },
    on: (event: string, fn: (p: VoiceState) => void) => {
      listeners.set(event, fn);
      return () => listeners.delete(event);
    },
  },
}));

const { voiceStore, VOICE_STATE_EMPTY } = await import("$lib/stores/voiceStore.svelte");
const settle = () => new Promise<void>((r) => queueMicrotask(r));

function state(overrides: Partial<VoiceState> = {}): VoiceState {
  return {
    ...VOICE_STATE_EMPTY,
    settings: { enabled: true, model: "base.en-q5_1", logTranscripts: false, cueVolume: 0.6 },
    ...overrides,
  };
}

describe("voiceStore", () => {
  test("the first subscriber reads voice.state and listens for the event", async () => {
    const release = voiceStore.subscribe();
    expect(reads).toBe(1);
    expect(listeners.has("voice.state")).toBe(true);
    answer?.(state({ state: "idle", device: "Yeti" }));
    await settle();
    expect(voiceStore.state.state).toBe("idle");
    expect(voiceStore.state.device).toBe("Yeti");
    release();
  });

  test("an event that lands while the first read is in flight wins over it", async () => {
    const release = voiceStore.subscribe();
    listeners.get("voice.state")?.(state({ state: "listening" }));
    answer?.(state({ state: "idle" }));
    await settle();
    expect(voiceStore.state.state).toBe("listening");
    release();
  });

  test("a second subscriber shares the feed; the last release empties the store", async () => {
    const readsBefore = reads;
    const a = voiceStore.subscribe();
    const b = voiceStore.subscribe();
    expect(reads).toBe(readsBefore + 1);
    listeners.get("voice.state")?.(state({ state: "thinking", transcript: "hello" }));
    a();
    a(); // a double release must not end b's feed
    expect(listeners.has("voice.state")).toBe(true);
    expect(voiceStore.state.transcript).toBe("hello");
    b();
    expect(listeners.has("voice.state")).toBe(false);
    expect(voiceStore.state).toEqual(VOICE_STATE_EMPTY);
  });

  test("a read that resolves after the last release is dropped", async () => {
    const release = voiceStore.subscribe();
    release();
    answer?.(state({ state: "idle" }));
    await settle();
    expect(voiceStore.state).toEqual(VOICE_STATE_EMPTY);
  });
});
