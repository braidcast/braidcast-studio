// Frontend-only Go Live preference: whether clicking Go Live opens the Stream
// Information modal (trigger option A). There is no backend GeneralSettings field
// for this, so it persists in localStorage (default ON). GeneralTab edits it; the
// Studio Go-Live bar reads it to choose modal vs instant start.

const KEY = "obs.askStreamInfoOnGoLive";

function load(): boolean {
  try {
    const v = localStorage.getItem(KEY);
    return v === null ? true : v === "1";
  } catch {
    return true;
  }
}

export const goLivePref = $state<{ askStreamInfo: boolean }>({ askStreamInfo: load() });

export function setAskStreamInfo(v: boolean): void {
  goLivePref.askStreamInfo = v;
  try {
    localStorage.setItem(KEY, v ? "1" : "0");
  } catch {
    // Non-fatal: the toggle still works for this session.
  }
}
