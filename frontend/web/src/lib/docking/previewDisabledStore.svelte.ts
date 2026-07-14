// Persisted "Disable Preview" toggle for preview surfaces (OBS parity: right-click
// the preview -> Disable Preview, to stop rendering it and save GPU). Frontend-only
// preference, no backend field, so it persists in localStorage the same way
// goLivePrefStore.svelte.ts persists askStreamInfo (default OFF, i.e. preview
// enabled). Keyed so both the Default PreviewDock and every per-canvas CanvasDock
// (one instance per canvasUuid) share this single store instead of forking it.

export const DEFAULT_PREVIEW_KEY = "__default__";

// The Default dock predates the keyed store and already shipped under the bare
// "obs.previewDisabled" key; keep that exact key so existing users who disabled it
// stay disabled. Every other key (a canvasUuid) gets its own namespaced key.
function storageKey(key: string): string {
  return key === DEFAULT_PREVIEW_KEY ? "obs.previewDisabled" : `obs.previewDisabled.${key}`;
}

// Non-reactive read-through cache: avoids re-hitting localStorage on every call
// for a key that hasn't been explicitly toggled yet (e.g. every reportRect/template
// read), without mutating the reactive `disabled` state during a render pass.
const cache = new Map<string, boolean>();

function load(key: string): boolean {
  if (cache.has(key)) {
    return cache.get(key)!;
  }
  let v = false;
  try {
    v = localStorage.getItem(storageKey(key)) === "1";
  } catch {
    v = false;
  }
  cache.set(key, v);
  return v;
}

const disabled = $state<Record<string, boolean>>({});

export function isPreviewDisabled(key: string): boolean {
  return key in disabled ? disabled[key] : load(key);
}

export function setPreviewDisabled(key: string, v: boolean): void {
  disabled[key] = v;
  cache.set(key, v);
  try {
    localStorage.setItem(storageKey(key), v ? "1" : "0");
  } catch {
    // Non-fatal: the toggle still works for this session.
  }
}
