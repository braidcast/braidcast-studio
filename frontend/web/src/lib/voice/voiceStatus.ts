// How voice control reads in the UI: one pure function per surface, so the wording and
// the tone are decided in tests rather than in markup. The Settings tab and the status
// indicator only render what these return.
import type {
  VoiceDraftAddress,
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
  /** Offer Cancel: whenever Confirm is offered, and also while a draft whose countdown
   * ran out waits for the segment being spoken (pending.due), since that draft can still
   * go out when the segment ends. */
  cancellable: boolean;
  /** The pending action those buttons act on (pending.id), 0 with none: every click names
   * it, so one that lands after the action is gone acts on nothing. */
  pendingId: number;
}

const MEGABYTE = 1024 * 1024;

/** The bridge method a dictated chat message is, reply or not. */
const CHAT_SEND = "chat.send";

/** True for a pending dictated chat message rather than a studio command. */
function isChatDraft(pending: VoicePendingAction | null): boolean {
  return pending !== null && pending.commandId === CHAT_SEND;
}

/** A window's length in whole seconds, for saying what the limit is. */
function windowSeconds(pending: VoicePendingAction): number {
  return Math.max(1, Math.round(pending.timeoutMs / 1000));
}

// What a pending item asks of the user. A draft is not confirmed the way a command is: in
// the countdown mode it goes on its own unless cancelled, and otherwise it waits for "send"
// for a limited time, after which it is not sent but kept for the composer. Every other
// pending command is confirmed by a spoken yes (or the buttons beside this): the registry
// asks for the word on every one, and a second key press confirms nothing.
function pendingDetail(pending: VoicePendingAction | null): string {
  if (pending && isChatDraft(pending)) {
    return pending.runOnTimeout ? "Sends unless you cancel" : `Say send within ${windowSeconds(pending)}s to post it`;
  }
  return "Say yes to confirm";
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

/** What a draft due mid-segment is waiting for (pending.due). */
const DUE_DETAIL = "Draft waits for what you say: send or cancel";

export function voiceIndicator(state: VoiceState): VoiceIndicator {
  if (!state.settings.enabled || state.state === "disabled") {
    return { visible: false, tone: "idle", label: "", detail: "", confirmable: false, cancellable: false, pendingId: 0 };
  }
  switch (state.state) {
    case "listening":
    case "thinking": {
      // A draft whose countdown ran out mid-segment waits for what is being said, and is
      // still on its way out: it keeps its Cancel, and the detail says why it is there.
      const due = state.pending?.due ? state.pending : null;
      const listening = state.state === "listening";
      return {
        visible: true,
        tone: listening ? "live" : "busy",
        label: listening ? "Listening" : "Thinking",
        detail: due ? DUE_DETAIL : listening ? state.device : state.transcript,
        confirmable: false,
        cancellable: due !== null,
        pendingId: due?.id ?? 0,
      };
    }
    case "pending":
      return {
        visible: true,
        tone: "warn",
        label: state.pending?.summary || "Waiting for confirmation",
        detail: pendingDetail(state.pending),
        confirmable: state.pending !== null,
        cancellable: state.pending !== null,
        pendingId: state.pending?.id ?? 0,
      };
    case "notReady":
      return {
        visible: true,
        tone: "warn",
        label: "Voice not ready",
        detail: readyDetail(state) || state.message,
        confirmable: false,
        cancellable: false,
        pendingId: 0,
      };
    default: {
      // Always-listen says what it is waiting for. Asked for but not running (its models
      // are missing, or did not load), push-to-talk still works, and the reason comes
      // first.
      const wake = state.settings.triggerMode === "wake";
      if (wake && state.ready.wake) {
        return {
          visible: true,
          tone: "idle",
          label: `Listening for \u201c${state.settings.wakePhrase}\u201d`,
          detail: state.message || state.device,
          confirmable: false,
          cancellable: false,
          pendingId: 0,
        };
      }
      return {
        visible: true,
        tone: "idle",
        label: "Voice ready",
        detail: (wake && state.ready.wakeReason) || state.message || state.device,
        confirmable: false,
        cancellable: false,
        pendingId: 0,
      };
    }
  }
}

/** The host's wake model (VoiceModels.hpp kWakeModelId): always-listen checks each
 * utterance's opening with it. */
export const WAKE_MODEL_ID = "tiny.en-q5_1";

export interface WakeModelNote {
  text: string;
  /** The model to offer a download for here; null when the model list already offers it. */
  model: VoiceModelStatus | null;
}

/** In always-listen, what the tab says about the wake model while it is not on disk; null
 * in push-to-talk or once it is ready. In English the model list below offers it; for any
 * other language it is not a choice of speech model, so the note carries the download. */
export function wakeModelNote(settings: VoiceSettingsState, models: VoiceModelStatus[]): WakeModelNote | null {
  if (settings.triggerMode !== "wake") {
    return null;
  }
  const tiny = models.find((m) => m.id === WAKE_MODEL_ID);
  if (!tiny || tiny.state === "ready") {
    return null;
  }
  return tiny.selectable
    ? { text: `Always-listen also needs ${tiny.label} below, for the wake phrase.`, model: null }
    : { text: `Always-listen also needs ${tiny.label}, for the wake phrase.`, model: tiny };
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

/** Where a draft stands: counting down to sending itself ("countdown"); waiting for the
 * word "send", after which it is kept for the composer rather than sent ("waiting"); or its
 * countdown ran out while a new segment was being spoken, and it waits for what that
 * segment says ("due"). */
export type VoiceDraftPhase = "countdown" | "waiting" | "due";

export interface VoiceDraft {
  /** The pending action this draft is (pending.id): Send now, Edit and Cancel name it. */
  id: number;
  /** The message exactly as it will be posted (a reply carries its "@name "). */
  text: string;
  /** Where it goes (pending.to). */
  to: VoiceDraftAddress | null;
  summary: string;
  phase: VoiceDraftPhase;
  /**
   * Milliseconds LEFT of the window, as of the host's last emit: until it sends itself
   * (countdown) or is taken off unsent (waiting); 0 while due. Deliberately not the
   * configured duration: the window is measured from a fixed deadline on the host, so
   * anything that delays the draft reaching here has already spent part of it. Rendering
   * the duration instead would show a full take-it-back window over one that is nearly,
   * or entirely, gone.
   */
  remainingMs: number;
  /** How long the window is, in whole seconds: the limit, for saying what it is. */
  limitSec: number;
}

/** The dictated chat message waiting to go out, if there is one. Shown whatever the
 * state says while it is pending: a new segment may be open (the key held to say
 * "cancel"), and a draft due mid-segment can still go out when that segment ends, so
 * neither it nor its Cancel may drop out of sight then. */
export function voiceDraft(state: VoiceState): VoiceDraft | null {
  const pending = state.pending;
  if (state.state === "disabled" || state.state === "notReady" || !pending || !isChatDraft(pending)) {
    return null;
  }
  const phase: VoiceDraftPhase = pending.due ? "due" : pending.runOnTimeout ? "countdown" : "waiting";
  return {
    id: pending.id,
    text: pending.text,
    to: pending.to,
    summary: pending.summary,
    phase,
    remainingMs: phase === "due" ? 0 : Math.max(0, pending.remainingMs),
    limitSec: windowSeconds(pending),
  };
}

/** Whole seconds left of a draft's window, `elapsedMs` after the host said `remainingMs`
 * were left: rounded up, so "1" shows until the very end, and never below 0. */
export function draftSecondsLeft(remainingMs: number, elapsedMs: number): number {
  return Math.max(0, Math.ceil((remainingMs - Math.max(0, elapsedMs)) / 1000));
}

/** The band's visible line: what happens next, and when. `seconds` is what is left now. */
export function draftBandLabel(draft: VoiceDraft, seconds: number): string {
  switch (draft.phase) {
    case "countdown":
      return `Sending in ${seconds}s`;
    case "waiting":
      return `Say \u201csend\u201d within ${seconds}s`;
    default:
      return "Waiting for you to finish";
  }
}

/** What the band's live region says. It depends on the draft and its phase only, never
 * on the seconds left, so a screen reader hears it once per draft and once per change of
 * phase, not every tick. It names the limit, and what happens when it runs out. */
export function draftAnnouncement(draft: VoiceDraft, destination = ""): string {
  const what = destination ? `Voice message ${destination}` : "Voice message";
  switch (draft.phase) {
    case "countdown":
      return `${what}: sends in ${draft.limitSec} seconds unless you cancel.`;
    case "waiting":
      return `${what}: say send within ${draft.limitSec} seconds, or it is not sent and moves to the composer.`;
    default:
      return `${what}: waiting for what you are saying. Say send or cancel.`;
  }
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
