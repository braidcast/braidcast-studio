// The UI's clicks on a pending voice action. Every one names the action it was made for
// (pending.id), because a click can land after that action is gone: the window closed, or
// a new command became pending in its place. The host refuses a click naming an action
// that is no longer the pending one and changes nothing, so a Send meant for an expired
// draft cannot confirm "Stop streaming?" instead.
import { obs, type VoiceDraftAddress } from "$lib/api/bridge";

/** Confirm pending action `id`, as a spoken yes would. True when the host took it; false
 * when it refused (the action is gone). A refusal is not an error to show: the next
 * voice.state already says what happened. */
export async function confirmVoice(id: number): Promise<boolean> {
  try {
    await obs.call("voice.confirm", { id });
    return true;
  } catch {
    return false;
  }
}

/** Drop pending action `id`. False when the host refused: that action is gone already
 * (sent, perhaps), and whatever is pending now is left alone. */
export async function cancelVoice(id: number): Promise<boolean> {
  try {
    await obs.call("voice.cancel", { id });
    return true;
  } catch {
    return false;
  }
}

/** A dictated draft taken out of voice's hands, for the composer. */
export interface TakenDraft {
  text: string;
  to: VoiceDraftAddress | null;
}

/** Take a pending draft over for editing: cancel it on the host first, and hand its text
 * over only once the host confirms the cancel. If its window closed first the message is
 * already out, and putting it in the composer as well would invite sending it twice, so
 * this answers null and the composer is left as it is. */
export async function takeOverDraft(draft: TakenDraft & { id: number }): Promise<TakenDraft | null> {
  if (!(await cancelVoice(draft.id))) {
    return null;
  }
  return { text: draft.text, to: draft.to };
}
