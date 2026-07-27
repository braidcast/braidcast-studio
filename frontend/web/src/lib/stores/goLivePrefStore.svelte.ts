// Frontend-only Go Live view preferences. Neither has a backend GeneralSettings
// field, so both persist in localStorage behind the one guarded reader/writer here.
//   askStreamInfo — clicking Go Live opens the Stream Information modal (trigger
//     option A). GeneralTab edits it; the Studio Go-Live bar reads it to choose
//     modal vs instant start.
//   hideDisabled  — the Stream Information modal hides channels whose output
//     bindings are all disabled (default ON: a destination switched off in the
//     Multistream dock shouldn't reappear here). Presentation only; it never
//     changes what streams.
// Adding a preference is one row in PREFS plus one line in the initial state.

const PREFS = {
  askStreamInfo: { key: "obs.askStreamInfoOnGoLive", fallback: true },
  hideDisabled: { key: "obs.goLiveHideDisabled", fallback: true },
} as const;

type PrefName = keyof typeof PREFS;

function load(name: PrefName): boolean {
  try {
    const v = localStorage.getItem(PREFS[name].key);
    return v === null ? PREFS[name].fallback : v === "1";
  } catch {
    return PREFS[name].fallback;
  }
}

export const goLivePref = $state<Record<PrefName, boolean>>({
  askStreamInfo: load("askStreamInfo"),
  hideDisabled: load("hideDisabled"),
});

export function setGoLivePref(name: PrefName, value: boolean): void {
  goLivePref[name] = value;
  try {
    localStorage.setItem(PREFS[name].key, value ? "1" : "0");
  } catch {
    // Non-fatal: the toggle still works for this session.
  }
}
