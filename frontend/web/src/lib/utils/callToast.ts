import { obs, type ObsMethods } from "$lib/api/bridge";
import { showToast } from "$lib/stores/toastStore.svelte";

// scenes.duplicate and scenes.duplicateToCanvas can return a name other than the one
// requested, when the requested one collided and the bridge suffixed it. This is the
// shared " as \"<actual>\"" clause toast messages append to report that -- empty when
// the names match.
export function renamedSuffix(requested: string, actual: string): string {
  return actual !== requested ? ` as "${actual}"` : "";
}

// overlays.test and events.replay share the same "0 delivered" meaning: the frame reached
// the overlay server and no eligible widget was listening for it. One place for the
// wording so the two callers (PreviewPane.svelte, EventsDock.svelte) cannot drift on what
// that means.
//
// Both lines open by confirming the send. That half is not decoration: the button gives no
// other sign it fired, so a message that only reports the absence reads as "the click did
// nothing" -- the one reading that sends a user looking for a bug instead of a listener.
// Same shape for both so the two stay legible side by side: <what> sent, but <who> received
// it — <what to do>.
//
// No `title`: the whole point is already fully visible in `message`, and a title repeating
// it buys a sighted user nothing (it is only a hover tooltip) while some screen readers take
// it as the element's description and read the sentence twice.
export function showNothingReceivedToast(kind: "test" | "replay"): void {
  const message =
    kind === "test"
      ? "Test sent, but nothing received it — no preview or Browser Source is connected."
      : "Replay sent, but no alert overlay received it — open a preview or add one to a live scene.";
  showToast(message, "");
}

// Await a bridge call and, on rejection, surface a concise transient toast instead
// of failing silently. Returns the result on success, or null on failure so the
// caller can keep its local state correct (revert an optimistic update, restore a
// draft, skip a follow-up). Use for DIRECT user actions only — never for
// background polls, which would spam the toast on every transient hiccup.
export async function callOrToast<K extends keyof ObsMethods>(
  method: K,
  params?: unknown,
  errPrefix?: string,
): Promise<ObsMethods[K] | null> {
  try {
    return await obs.call(method, params);
  } catch (e) {
    const msg = (e as Error).message;
    showToast(errPrefix ? errPrefix + ": " + msg : msg, msg);
    return null;
  }
}
