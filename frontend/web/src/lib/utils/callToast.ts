import { obs, type ObsMethods } from "$lib/api/bridge";
import { showToast } from "$lib/stores/toastStore.svelte";

// scenes.duplicate and scenes.duplicateToCanvas can return a name other than the one
// requested, when the requested one collided and the bridge suffixed it. This is the
// shared " as \"<actual>\"" clause toast messages append to report that -- empty when
// the names match.
export function renamedSuffix(requested: string, actual: string): string {
  return actual !== requested ? ` as "${actual}"` : "";
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
