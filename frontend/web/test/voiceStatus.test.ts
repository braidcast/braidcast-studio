import { describe, expect, it } from "bun:test";
import type { VoicePendingAction, VoiceSettingsState } from "$lib/api/bridge";
import {
  composerWithDraft,
  draftAnnouncement,
  draftBandLabel,
  draftSecondsLeft,
  keptDraftToFill,
  voiceDraft,
  voiceEnableGate,
  voiceIndicator,
  voiceModelLabel,
  wakeModelNote,
  type VoiceModelStatus,
  type VoiceState,
} from "$lib/voice/voiceStatus";

const READY: VoiceState["ready"] = { cpu: true, cpuReason: "", model: true, mic: true, wake: false, wakeReason: "" };

function state(overrides: Partial<VoiceState> = {}): VoiceState {
  return {
    state: "idle",
    message: "",
    transcript: "",
    device: "Microphone (Yeti)",
    pending: null,
    keptDraft: "",
    keptTo: null,
    keptId: 0,
    ready: READY,
    settings: {
      enabled: true,
      model: "base.en-q5_1",
      logTranscripts: false,
      cueVolume: 0.6,
      sendMode: "countdown",
      countdownSec: 3,
      triggerMode: "ptt",
      wakePhrase: "Braidcast",
      readBack: false,
      language: "en",
    },
    ...overrides,
  };
}

function pending(overrides: Partial<VoicePendingAction> = {}): VoicePendingAction {
  return {
    id: 1,
    commandId: "streaming.stop",
    summary: "Stop streaming?",
    needsConfirmWord: true,
    deadlineMs: 1_000_000,
    remainingMs: 8000,
    runOnTimeout: false,
    due: false,
    timeoutMs: 0,
    text: "",
    to: null,
    ...overrides,
  };
}

function model(overrides: Partial<VoiceModelStatus> = {}): VoiceModelStatus {
  return {
    id: "base.en-q5_1",
    label: "Base (English)",
    kind: "speech",
    selectable: true,
    multilingual: false,
    bytes: 59721011,
    received: 0,
    state: "absent",
    ...overrides,
  };
}

const off: VoiceSettingsState = {
  enabled: false,
  model: "base.en-q5_1",
  logTranscripts: false,
  cueVolume: 0.6,
  sendMode: "countdown",
  countdownSec: 3,
  triggerMode: "ptt",
  wakePhrase: "Braidcast",
  readBack: false,
  language: "en",
};

describe("voiceIndicator", () => {
  it("says it is listening for the wake phrase when idle in wake mode", () => {
    const indicator = voiceIndicator(
      state({
        ready: { ...READY, wake: true },
        settings: {
          enabled: true,
          model: "base.en-q5_1",
          logTranscripts: false,
          cueVolume: 0.6,
          sendMode: "countdown",
          countdownSec: 3,
          triggerMode: "wake",
          wakePhrase: "hey braidcast",
          readBack: false,
          language: "en",
        },
      }),
    );
    expect(indicator.label).toBe("Listening for \u201chey braidcast\u201d");
    expect(indicator.tone).toBe("idle");
  });

  it("in wake mode without always-listen running, is ready for the key and says why", () => {
    const indicator = voiceIndicator(
      state({
        ready: { ...READY, wake: false, wakeReason: "Always-listen needs the Tiny (English) model too." },
        settings: { ...state().settings, triggerMode: "wake" },
      }),
    );
    expect(indicator.label).toBe("Voice ready");
    expect(indicator.detail).toBe("Always-listen needs the Tiny (English) model too.");
  });

  it("ignores always-listen fields in push-to-talk", () => {
    const indicator = voiceIndicator(state({ ready: { ...READY, wake: true, wakeReason: "stale" } }));
    expect(indicator.label).toBe("Voice ready");
    expect(indicator.detail).toBe("Microphone (Yeti)");
  });

  it("is hidden while voice is off", () => {
    expect(voiceIndicator(state({ state: "disabled", settings: off })).visible).toBe(false);
  });

  it("is hidden when the setting is off even if a stale state says otherwise", () => {
    expect(voiceIndicator(state({ state: "idle", settings: off })).visible).toBe(false);
    expect(voiceIndicator(state({ state: "disabled" })).visible).toBe(false);
  });

  it("reads idle as ready, with the device name", () => {
    const indicator = voiceIndicator(state());
    expect(indicator).toEqual({
      visible: true,
      tone: "idle",
      label: "Voice ready",
      detail: "Microphone (Yeti)",
      confirmable: false,
      cancellable: false,
      pendingId: 0,
    });
  });

  it("reads listening as live", () => {
    const indicator = voiceIndicator(state({ state: "listening" }));
    expect(indicator.tone).toBe("live");
    expect(indicator.label).toBe("Listening");
    expect(indicator.detail).toBe("Microphone (Yeti)");
  });

  it("shows the transcript while thinking, once there is one", () => {
    expect(voiceIndicator(state({ state: "thinking" })).label).toBe("Thinking");
    expect(voiceIndicator(state({ state: "thinking" })).tone).toBe("busy");
    expect(voiceIndicator(state({ state: "thinking", transcript: "switch to gameplay" })).detail).toBe(
      "switch to gameplay",
    );
  });

  it("shows the pending summary and marks it as needing an answer", () => {
    const indicator = voiceIndicator(state({ state: "pending", pending: pending() }));
    expect(indicator.tone).toBe("warn");
    expect(indicator.label).toBe("Stop streaming?");
    expect(indicator.detail).toBe("Say yes to confirm");
  });

  it("never asks for a second key press, which confirms nothing", () => {
    const indicator = voiceIndicator(state({ state: "pending", pending: pending({ needsConfirmWord: false }) }));
    expect(indicator.detail).toBe("Say yes to confirm");
  });

  it("keeps Cancel, and only Cancel, for a draft due while a segment is spoken", () => {
    const due = pending({ id: 9, commandId: "chat.send", runOnTimeout: true, due: true, remainingMs: 0, text: "gg" });
    for (const s of ["listening", "thinking"] as const) {
      const indicator = voiceIndicator(state({ state: s, pending: due }));
      expect(indicator.cancellable).toBe(true);
      expect(indicator.confirmable).toBe(false);
      expect(indicator.pendingId).toBe(9);
      expect(indicator.detail).toContain("send or cancel");
    }
    const notDue = voiceIndicator(state({ state: "listening", pending: { ...due, due: false } }));
    expect(notDue.cancellable).toBe(false);
    expect(notDue.detail).toBe("Microphone (Yeti)");
  });

  it("still says something when pending arrives without the action", () => {
    expect(voiceIndicator(state({ state: "pending", pending: null })).label).toBe("Waiting for confirmation");
  });

  it("offers buttons while a command is pending, for that action by its id", () => {
    const indicator = voiceIndicator(state({ state: "pending", pending: pending({ id: 42 }) }));
    expect(indicator.confirmable).toBe(true);
    expect(indicator.pendingId).toBe(42);
  });

  it("offers no buttons when nothing is pending", () => {
    expect(voiceIndicator(state()).confirmable).toBe(false);
    for (const s of ["listening", "thinking", "notReady", "disabled"] as const) {
      expect(voiceIndicator(state({ state: s })).confirmable).toBe(false);
    }
  });

  it("offers no buttons for a pending state that carries no action to confirm", () => {
    expect(voiceIndicator(state({ state: "pending", pending: null })).confirmable).toBe(false);
  });

  it("explains an unsupported CPU rather than saying not ready", () => {
    const indicator = voiceIndicator(
      state({
        state: "notReady",
        ready: { ...READY, cpu: false, cpuReason: "Voice control needs a CPU with AVX2 support.", model: false },
      }),
    );
    expect(indicator.tone).toBe("warn");
    expect(indicator.label).toBe("Voice not ready");
    expect(indicator.detail).toBe("Voice control needs a CPU with AVX2 support.");
  });

  it("names the missing piece when not ready", () => {
    expect(
      voiceIndicator(state({ state: "notReady", ready: { ...READY, mic: false } })).detail,
    ).toBe("No microphone on the Mic/Aux channel");
    expect(
      voiceIndicator(state({ state: "notReady", ready: { ...READY, model: false } })).detail,
    ).toBe("The speech model is not ready");
  });

  it("names the mic before the model when both are missing", () => {
    expect(
      voiceIndicator(state({ state: "notReady", ready: { ...READY, model: false, mic: false } })).detail,
    ).toBe("No microphone on the Mic/Aux channel");
  });

  it("keeps the host's reason for a model that failed", () => {
    expect(
      voiceIndicator(
        state({
          state: "notReady",
          message: "Base (English) model is not downloaded",
          ready: { ...READY, model: false },
        }),
      ).detail,
    ).toBe("Base (English) model is not downloaded");
  });

  it("prefers the host message when there is one", () => {
    expect(voiceIndicator(state({ state: "idle", message: "No scene called BRB." })).detail).toBe(
      "No scene called BRB.",
    );
  });
});

describe("voiceModelLabel", () => {
  it("shows progress while downloading", () => {
    expect(voiceModelLabel(model({ bytes: 100, received: 25, state: "downloading" }))).toBe("Downloading 25%");
  });

  it("never shows more than 100% or divides by zero", () => {
    expect(voiceModelLabel(model({ bytes: 100, received: 140, state: "downloading" }))).toBe("Downloading 100%");
    expect(voiceModelLabel(model({ bytes: 0, received: 0, state: "downloading" }))).toBe("Downloading 0%");
  });

  it("shows the size when absent, and nothing when ready", () => {
    expect(voiceModelLabel(model())).toBe("57 MB download");
    expect(voiceModelLabel(model({ received: 59721011, state: "ready" }))).toBe("Ready");
  });

  it("rounds a small model up to 1 MB rather than 0", () => {
    expect(voiceModelLabel(model({ bytes: 885098, kind: "vad", selectable: false }))).toBe("1 MB download");
  });

  it("shows the reason it failed", () => {
    expect(voiceModelLabel(model({ bytes: 100, state: "failed", error: "hash mismatch" }))).toBe(
      "Failed: hash mismatch",
    );
    expect(voiceModelLabel(model({ state: "failed" }))).toBe("Failed: unknown error");
  });
});

describe("voiceEnableGate", () => {
  const cpuOk = { supported: true, reason: "" };
  const offBase: VoiceSettingsState = {
    enabled: false,
    model: "base.en-q5_1",
    logTranscripts: false,
    cueVolume: 0.6,
    sendMode: "countdown",
    countdownSec: 3,
    triggerMode: "ptt",
    wakePhrase: "Braidcast",
    readBack: false,
    language: "en",
  };

  it("allows turning on once the chosen model is on disk", () => {
    expect(voiceEnableGate(offBase, cpuOk, [model({ state: "ready" })])).toEqual({ blocked: false, why: "" });
  });

  it("blocks turning on before the chosen model is downloaded, and says so", () => {
    const gate = voiceEnableGate(offBase, cpuOk, [
      model({ state: "downloading" }),
      model({ id: "tiny.en-q5_1", state: "ready" }),
    ]);
    expect(gate.blocked).toBe(true);
    expect(gate.why).toBe("Download the selected model to turn voice control on.");
  });

  it("blocks turning on with the CPU's own reason", () => {
    const gate = voiceEnableGate(offBase, { supported: false, reason: "Needs AVX2." }, [model({ state: "ready" })]);
    expect(gate).toEqual({ blocked: true, why: "Needs AVX2." });
  });

  it("never blocks turning off", () => {
    const on = { ...offBase, enabled: true };
    expect(voiceEnableGate(on, { supported: false, reason: "Needs AVX2." }, [])).toEqual({ blocked: false, why: "" });
  });
});

describe("voiceDraft", () => {
  const draft = pending({
    id: 3,
    commandId: "chat.send",
    summary: "Send to chat: hello everyone",
    needsConfirmWord: false,
    runOnTimeout: true,
    timeoutMs: 3000,
    remainingMs: 3000,
    text: "hello everyone",
    to: { platforms: ["twitch", "youtube"] },
  });
  const held = { ...draft, runOnTimeout: false, needsConfirmWord: true, timeoutMs: 8000, remainingMs: 8000 };

  it("is nothing unless a chat draft is pending", () => {
    expect(voiceDraft(state())).toBeNull();
    expect(voiceDraft(state({ state: "pending", pending: pending() }))).toBeNull();
    expect(voiceDraft(state({ state: "disabled", pending: draft }))).toBeNull();
    expect(voiceDraft(state({ state: "notReady", pending: draft }))).toBeNull();
  });

  it("carries the id, the text, its address and the countdown", () => {
    const shown = voiceDraft(state({ state: "pending", pending: draft }));
    expect(shown).toEqual({
      id: 3,
      text: "hello everyone",
      to: { platforms: ["twitch", "youtube"] },
      summary: "Send to chat: hello everyone",
      phase: "countdown",
      remainingMs: 3000,
      limitSec: 3,
    });
  });

  it("counts what is left, not how long the window was", () => {
    const shown = voiceDraft(state({ state: "pending", pending: { ...draft, remainingMs: 900 } }));
    expect(shown?.remainingMs).toBe(900);
    expect(shown?.limitSec).toBe(3);
  });

  it("shows no window at all when the deadline is already spent", () => {
    expect(voiceDraft(state({ state: "pending", pending: { ...draft, remainingMs: 0 } }))?.remainingMs).toBe(0);
    expect(voiceDraft(state({ state: "pending", pending: { ...draft, remainingMs: -50 } }))?.remainingMs).toBe(0);
  });

  it("waits for the word with a limit, the default window included", () => {
    const shown = voiceDraft(state({ state: "pending", pending: { ...held, remainingMs: 6100 } }));
    expect(shown?.phase).toBe("waiting");
    expect(shown?.remainingMs).toBe(6100);
    expect(shown?.limitSec).toBe(8);
  });

  // W-13: the band and its Cancel stay while a new segment is open, and above all while a
  // countdown that ran out mid-segment waits for it: that draft can still go out.
  it("stays on screen while a segment is open, and is due once its countdown ran out then", () => {
    for (const s of ["listening", "thinking", "idle"] as const) {
      expect(voiceDraft(state({ state: s, pending: draft }))?.phase).toBe("countdown");
      const due = voiceDraft(state({ state: s, pending: { ...draft, due: true, remainingMs: 0 } }));
      expect(due?.phase).toBe("due");
      expect(due?.remainingMs).toBe(0);
      expect(due?.id).toBe(3);
    }
  });

  it("tells the indicator a draft is not confirmed like a command, and names the limit", () => {
    expect(voiceIndicator(state({ state: "pending", pending: draft })).detail).toBe("Sends unless you cancel");
    expect(voiceIndicator(state({ state: "pending", pending: held })).detail).toBe("Say send within 8s to post it");
  });
});

describe("draftBandLabel and draftAnnouncement", () => {
  const base = { id: 1, text: "gg", to: null, summary: "", remainingMs: 3000, limitSec: 3 };

  it("says what happens next, with the seconds left", () => {
    expect(draftBandLabel({ ...base, phase: "countdown" }, 2)).toBe("Sending in 2s");
    expect(draftBandLabel({ ...base, phase: "waiting", limitSec: 8 }, 6)).toBe("Say \u201csend\u201d within 6s");
    expect(draftBandLabel({ ...base, phase: "due", remainingMs: 0 }, 0)).toBe("Waiting for you to finish");
  });

  it("announces the limit and what happens at its end, and no ticking seconds", () => {
    expect(draftAnnouncement({ ...base, phase: "countdown" })).toBe(
      "Voice message: sends in 3 seconds unless you cancel.",
    );
    const waiting = draftAnnouncement({ ...base, phase: "waiting", limitSec: 8 }, "to Twitch chat");
    expect(waiting).toBe(
      "Voice message to Twitch chat: say send within 8 seconds, or it is not sent and moves to the composer.",
    );
    // The same sentence whatever is left, so the live region is not re-read as it counts.
    expect(draftAnnouncement({ ...base, phase: "countdown", remainingMs: 1200 })).toBe(
      draftAnnouncement({ ...base, phase: "countdown", remainingMs: 2900 }),
    );
    expect(draftAnnouncement({ ...base, phase: "due" })).toContain("Say send or cancel");
  });
});

describe("draftSecondsLeft", () => {
  it("rounds up, so the last second still reads 1", () => {
    expect(draftSecondsLeft(3000, 0)).toBe(3);
    expect(draftSecondsLeft(3000, 1)).toBe(3);
    expect(draftSecondsLeft(3000, 2001)).toBe(1);
    expect(draftSecondsLeft(3000, 2999)).toBe(1);
  });

  it("never goes below zero, and ignores a clock that ran backwards", () => {
    expect(draftSecondsLeft(3000, 3000)).toBe(0);
    expect(draftSecondsLeft(3000, 9000)).toBe(0);
    expect(draftSecondsLeft(0, 0)).toBe(0);
    expect(draftSecondsLeft(2500, -400)).toBe(3);
  });
});

describe("composerWithDraft", () => {
  it("fills an empty composer", () => {
    expect(composerWithDraft("", "hello")).toBe("hello");
    expect(composerWithDraft("   ", "hello")).toBe("hello");
  });

  it("never overwrites what is typed, it follows it", () => {
    expect(composerWithDraft("so anyway", "hello")).toBe("so anyway hello");
    expect(composerWithDraft("so anyway  ", "hello")).toBe("so anyway hello");
  });
});

describe("keptDraftToFill", () => {
  it("hands a refused message to an empty composer once", () => {
    expect(keptDraftToFill("a long message", "", "")).toBe("a long message");
    expect(keptDraftToFill("a long message", "a long message", "")).toBeNull();
  });

  it("leaves a composer the user is typing in alone", () => {
    expect(keptDraftToFill("a long message", "", "typing")).toBeNull();
  });

  it("does nothing without one", () => {
    expect(keptDraftToFill("", "", "")).toBeNull();
  });
});

describe("wakeModelNote", () => {
  const tiny = (overrides: Partial<VoiceModelStatus> = {}) =>
    model({ id: "tiny.en-q5_1", label: "Tiny (English, fastest)", bytes: 32166155, ...overrides });
  const wakeMode: VoiceSettingsState = { ...off, enabled: true, triggerMode: "wake" };

  it("says nothing in push-to-talk", () => {
    expect(wakeModelNote({ ...wakeMode, triggerMode: "ptt" }, [tiny()])).toBeNull();
  });

  it("says nothing once the wake model is on disk", () => {
    expect(wakeModelNote(wakeMode, [tiny({ state: "ready" })])).toBeNull();
  });

  it("points at the model list in English, where the model is offered", () => {
    expect(wakeModelNote(wakeMode, [tiny()])).toEqual({
      text: "Always-listen also needs Tiny (English, fastest) below, for the wake phrase.",
      model: null,
    });
  });

  it("carries the download itself in another language, where the list does not offer it", () => {
    const hidden = tiny({ selectable: false, state: "downloading", received: 1000 });
    const note = wakeModelNote({ ...wakeMode, language: "de" }, [hidden]);
    expect(note?.model).toEqual(hidden);
    expect(note?.text).toBe("Always-listen also needs Tiny (English, fastest), for the wake phrase.");
  });

  it("says nothing when the catalog has no wake model", () => {
    expect(wakeModelNote(wakeMode, [model()])).toBeNull();
  });
});
