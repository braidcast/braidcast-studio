// How voice control reads in the UI: one pure function per surface, so the wording and
// the tone are decided in tests rather than in markup. The Settings tab and the status
// indicator only render what these return.
import type {
  VoicePayload,
  VoiceModelStatus,
  VoicePendingAction,
  VoiceSettingsState,
  VoiceState,
} from "$lib/api/bridge";

export type { VoiceModelStatus, VoiceState };

export type VoiceTone = "idle" | "live" | "warn" | "busy";

export interface VoiceIndicator {
  visible: boolean;
  tone: VoiceTone;
  label: string;
  detail: string;
  /** A command is waiting: offer Confirm and Cancel beside the spoken yes. */
  confirmable: boolean;
}

const MEGABYTE = 1024 * 1024;

/** The bridge method a dictated chat message is, reply or not. */
const CHAT_SEND = "chat.send";

/** True for a pending dictated chat message rather than a studio command. */
function isChatDraft(pending: VoicePendingAction | null): boolean {
  return pending !== null && pending.commandId === CHAT_SEND;
}

// What a pending item asks of the user. A draft is not confirmed the way a command is: in
// the countdown mode it goes on its own unless cancelled, and otherwise it waits for "send".
function pendingDetail(pending: VoicePendingAction | null): string {
  if (pending && isChatDraft(pending)) {
    return pending.runOnTimeout ? "Sends unless you cancel" : "Say send to post it";
  }
  return pending?.needsConfirmWord ? "Say yes to confirm" : "Press the key again to confirm";
}

// What is missing, most fundamental first: a CPU that cannot run voice outranks
// everything, and a mic is named before the model because it is the user's to fix in
// the mixer. A model failure keeps the host's own reason (not downloaded, could not
// load) when it sent one.
function readyDetail(state: VoiceState): string {
  if (!state.ready.cpu) {
    return state.ready.cpuReason;
  }
  if (!state.ready.mic) {
    return "No microphone on the Mic/Aux channel";
  }
  if (!state.ready.model) {
    return state.message || "The speech model is not ready";
  }
  return "";
}

export function voiceIndicator(state: VoiceState): VoiceIndicator {
  if (!state.settings.enabled || state.state === "disabled") {
    return { visible: false, tone: "idle", label: "", detail: "", confirmable: false };
  }
  switch (state.state) {
    case "listening":
      return { visible: true, tone: "live", label: "Listening", detail: state.device, confirmable: false };
    case "thinking":
      return { visible: true, tone: "busy", label: "Thinking", detail: state.transcript, confirmable: false };
    case "pending":
      return {
        visible: true,
        tone: "warn",
        label: state.pending?.summary || "Waiting for confirmation",
        detail: pendingDetail(state.pending),
        confirmable: state.pending !== null,
      };
    case "notReady":
      return {
        visible: true,
        tone: "warn",
        label: "Voice not ready",
        detail: readyDetail(state) || state.message,
        confirmable: false,
      };
    default:
      return {
        visible: true,
        tone: "idle",
        label: "Voice ready",
        detail: state.message || state.device,
        confirmable: false,
      };
  }
}

export function voiceModelLabel(model: VoiceModelStatus): string {
  switch (model.state) {
    case "downloading": {
      const percent = model.bytes > 0 ? Math.min(100, Math.round((model.received / model.bytes) * 100)) : 0;
      return `Downloading ${percent}%`;
    }
    case "ready":
      return "Ready";
    case "failed":
      return `Failed: ${model.error || "unknown error"}`;
    default:
      return `${Math.max(1, Math.round(model.bytes / MEGABYTE))} MB download`;
  }
}

export interface VoiceEnableGate {
  /** The switch cannot be turned on. */
  blocked: boolean;
  /** Why, for the line under it; "" when nothing blocks. */
  why: string;
}

/** Whether the Settings switch may turn voice ON: the host refuses it on a CPU without
 * AVX2 and before the chosen model is on disk. Turning it OFF is never blocked, or a
 * model deleted (or a CPU swapped) under an enabled setting would lock voice on. */
export function voiceEnableGate(
  settings: VoiceSettingsState,
  cpu: VoicePayload["cpu"],
  models: VoiceModelStatus[],
): VoiceEnableGate {
  if (settings.enabled) {
    return { blocked: false, why: "" };
  }
  if (!cpu.supported) {
    return { blocked: true, why: cpu.reason };
  }
  if (!models.some((m) => m.id === settings.model && m.state === "ready")) {
    return { blocked: true, why: "Download the selected model to turn voice control on." };
  }
  return { blocked: false, why: "" };
}

export interface VoiceDraft {
  /** The message exactly as it will be posted (a reply carries its "@name "). */
  text: string;
  summary: string;
  /**
   * Milliseconds LEFT until it sends itself, as of the host's last emit, or 0 when it is
   * waiting for a word. Deliberately not the configured duration: the window is measured
   * from a fixed deadline on the host, so anything that delays the draft reaching here
   * has already spent part of it. Rendering the duration instead would show a full
   * take-it-back window over one that is nearly, or entirely, gone.
   */
  countdownMs: number;
  waitingForWord: boolean;
}

/** The dictated chat message waiting to go out, if there is one. */
export function voiceDraft(state: VoiceState): VoiceDraft | null {
  const pending = state.pending;
  if (state.state !== "pending" || !pending || !isChatDraft(pending)) {
    return null;
  }
  return {
    text: pending.text,
    summary: pending.summary,
    countdownMs: pending.runOnTimeout ? Math.max(0, pending.remainingMs) : 0,
    waitingForWord: !pending.runOnTimeout,
  };
}

/** Whole seconds left of a draft's countdown, `elapsedMs` after the host said `countdownMs`
 * were left: rounded up, so "1" shows until the very end, and never below 0. */
export function draftSecondsLeft(countdownMs: number, elapsedMs: number): number {
  return Math.max(0, Math.ceil((countdownMs - Math.max(0, elapsedMs)) / 1000));
}

/** A dictated message taken into the composer: in place of an empty one, or after what is
 * already typed there, never over it. */
export function composerWithDraft(composer: string, text: string): string {
  if (composer.trim() === "") {
    return text;
  }
  return composer.replace(/\s+$/, "") + " " + text;
}

/** The message voice refused as too long, for a composer to take over: the kept draft when
 * it is one this composer has not acted on yet and the composer is empty, else null. A
 * composer the user is typing in is never overwritten. */
export function keptDraftToFill(kept: string, seen: string, composer: string): string | null {
  if (kept === "" || kept === seen || composer.trim() !== "") {
    return null;
  }
  return kept;
}
