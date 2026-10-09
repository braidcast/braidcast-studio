// How voice control reads in the UI: one pure function per surface, so the wording and
// the tone are decided in tests rather than in markup. The Settings tab and the status
// indicator only render what these return.
import type { VoicePayload, VoiceModelStatus, VoiceSettingsState, VoiceState } from "$lib/api/bridge";

export type { VoiceModelStatus, VoiceState };

export type VoiceTone = "idle" | "live" | "warn" | "busy";

export interface VoiceIndicator {
  visible: boolean;
  tone: VoiceTone;
  label: string;
  detail: string;
}

const MEGABYTE = 1024 * 1024;

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
    return { visible: false, tone: "idle", label: "", detail: "" };
  }
  switch (state.state) {
    case "listening":
      return { visible: true, tone: "live", label: "Listening", detail: state.device };
    case "thinking":
      return { visible: true, tone: "busy", label: "Thinking", detail: state.transcript };
    case "pending":
      return {
        visible: true,
        tone: "warn",
        label: state.pending?.summary || "Waiting for confirmation",
        detail: state.pending?.needsConfirmWord ? "Say yes to confirm" : "Press the key again to confirm",
      };
    case "notReady":
      return { visible: true, tone: "warn", label: "Voice not ready", detail: readyDetail(state) || state.message };
    default:
      return { visible: true, tone: "idle", label: "Voice ready", detail: state.message || state.device };
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
