// The Duplicate Scene dialog's remembered Sources choice. Frontend-only, with no
// backend field, so it persists in localStorage the same way goLivePrefStore does
// (default "copy": independent copies).

import type { SceneDuplicateSources } from "$lib/api/bridge";

const KEY = "obs.duplicateSceneSources";
const FALLBACK: SceneDuplicateSources = "copy";

function load(): SceneDuplicateSources {
  try {
    const v = localStorage.getItem(KEY);
    return v === "copy" || v === "share" ? v : FALLBACK;
  } catch {
    return FALLBACK;
  }
}

export const duplicateScenePref = $state<{ sources: SceneDuplicateSources }>({ sources: load() });

export function setDuplicateSceneSources(value: SceneDuplicateSources): void {
  duplicateScenePref.sources = value;
  try {
    localStorage.setItem(KEY, value);
  } catch {
    // Non-fatal: the choice still holds for this session.
  }
}
