// Recently-used source TYPE ids for the Add Source picker. Frontend-only (no
// backend field), persisted in localStorage. Most-recent-first, capped.
const KEY = "obs.addSourceRecent";
const CAP = 6;

function load(): string[] {
  try {
    const v = localStorage.getItem(KEY);
    if (!v) return [];
    const parsed: unknown = JSON.parse(v);
    return Array.isArray(parsed) ? parsed.filter((x): x is string => typeof x === "string").slice(0, CAP) : [];
  } catch {
    return [];
  }
}

export const recentSources = $state<{ ids: string[] }>({ ids: load() });

export function pushRecent(typeId: string): void {
  const next = [typeId, ...recentSources.ids.filter((id) => id !== typeId)].slice(0, CAP);
  recentSources.ids = next;
  try {
    localStorage.setItem(KEY, JSON.stringify(next));
  } catch {
    // Non-fatal: the MRU still works for this session.
  }
}
