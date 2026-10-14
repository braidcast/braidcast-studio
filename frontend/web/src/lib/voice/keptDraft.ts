// A dictated message voice kept rather than sent (refused as too long, or a "say send"
// draft whose window closed) lands in an empty Multichat composer, once. "Once" is decided
// here, at module scope, not per component: two mounted docks must not both take it, and a
// dock that unmounts and mounts again (a layout change, a tab switch) must not take it a
// second time. Each kept draft has its own id (voice.state's keptId), which is what tells a
// draft already taken over from a new one with the same words.
import type { VoiceDraftAddress, VoiceState } from "$lib/api/bridge";

/** The keptId some composer already took over; 0 for none. */
let taken = 0;

export interface KeptDraft {
  text: string;
  /** Where it was going (keptTo): the composer sends it there. */
  to: VoiceDraftAddress | null;
}

/** The kept draft for this composer to take over, or null: when there is one no composer
 * has taken yet and this composer is empty. Never over what is typed, and a composer that
 * is busy leaves it for another one, so it is not marked taken then. */
export function takeKeptDraft(state: VoiceState, composer: string): KeptDraft | null {
  if (state.keptId === 0 || state.keptDraft === "") {
    // The host cleared it. A new listener numbers its kept drafts from 1 again, so forget
    // the last one then; but not on the store's placeholder state (voice "disabled"), which
    // a remounting dock can see before the host answers with the draft still kept.
    if (state.state !== "disabled") {
      taken = 0;
    }
    return null;
  }
  if (state.keptId === taken || composer.trim() !== "") {
    return null;
  }
  taken = state.keptId;
  return { text: state.keptDraft, to: state.keptTo };
}

/** Tests only: forget which kept draft was taken. */
export function resetKeptDraftForTests(): void {
  taken = 0;
}
