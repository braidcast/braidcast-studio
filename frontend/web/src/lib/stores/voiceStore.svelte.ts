// Voice control's live state, for the status-bar indicator and anything else that shows
// it. voice.state carries the whole state on every change, so an event is a complete
// replacement, and the first read (voice.state, the method) only fills in until one
// arrives.
//
// Ref-counted like viewerCountStore: the feed runs only while something mounted is
// showing it, and releasing the last reference drops back to "disabled" so the next
// subscriber shows nothing stale before the host answers.

import { obs } from "$lib/api/bridge";
import type { VoiceState } from "$lib/api/bridge";
import { RefCountedSubscription } from "$lib/stores/refCountedSubscription";
import { EV } from "$lib/utils/eventNames";

/** What the store holds with no feed: voice off, nothing to show. */
export const VOICE_STATE_EMPTY: VoiceState = {
  state: "disabled",
  message: "",
  transcript: "",
  device: "",
  pending: null,
  keptDraft: "",
  ready: { cpu: true, cpuReason: "", model: false, mic: false, wake: false, wakeReason: "" },
  settings: {
    enabled: false,
    model: "",
    logTranscripts: false,
    cueVolume: 0,
    sendMode: "countdown",
    countdownSec: 3,
    triggerMode: "ptt",
    wakePhrase: "",
    readBack: false,
    language: "en",
  },
};

class VoiceStore {
  #state = $state<VoiceState>(VOICE_STATE_EMPTY);
  // Bumped by every event and every read, so a read that resolves after a newer event
  // (or after the feed was released) describes an older state and is dropped.
  #seq = 0;

  #feed = new RefCountedSubscription(() => {
    const off = obs.on(EV.voiceState, (next) => {
      this.#seq++;
      this.#state = next;
    });
    void this.#read();
    return () => {
      off();
      this.#seq++;
      this.#state = VOICE_STATE_EMPTY;
    };
  });

  /** The latest voice.state; VOICE_STATE_EMPTY while nothing is subscribed. */
  get state(): VoiceState {
    return this.#state;
  }

  /** Take a reference on the feed; returns a release that is safe to call twice. */
  subscribe(): () => void {
    return this.#feed.subscribe();
  }

  async #read(): Promise<void> {
    const seq = ++this.#seq;
    try {
      const next = await obs.call("voice.state");
      if (seq === this.#seq) {
        this.#state = next;
      }
    } catch {
      // The host answers once it is up; the event feed fills in either way.
    }
  }
}

export const voiceStore = new VoiceStore();
