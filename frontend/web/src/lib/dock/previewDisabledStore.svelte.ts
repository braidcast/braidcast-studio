// Persisted "Disable Preview" toggle for the Default PreviewDock (OBS parity:
// right-click the preview -> Disable Preview, to stop rendering it and save GPU).
// Frontend-only preference, no backend field, so it persists in localStorage the
// same way goLivePrefStore.svelte.ts persists askStreamInfo (default OFF, i.e.
// preview enabled).

const KEY = "obs.previewDisabled";

function load(): boolean {
  try {
    return localStorage.getItem(KEY) === "1";
  } catch {
    return false;
  }
}

export const previewDisabledPref = $state<{ disabled: boolean }>({ disabled: load() });

export function setPreviewDisabled(v: boolean): void {
  previewDisabledPref.disabled = v;
  try {
    localStorage.setItem(KEY, v ? "1" : "0");
  } catch {
    // Non-fatal: the toggle still works for this session.
  }
}
