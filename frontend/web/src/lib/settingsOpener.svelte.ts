// Shared navigation state for the Settings page (7.3). The Settings UI is now a
// full nav-rail page, not a modal: openSettings(tab) navigates the page router to
// it with a tab selected (and, for "canvases", a canvas pre-opened for edit).
// SettingsPage reads settingsNav; the sub-nav buttons call setSettingsTab.

import { setPage } from "./pageStore.svelte";

export type SettingsTab =
  | "general"
  | "canvases"
  | "audio"
  | "hotkeys"
  | "browserDocks"
  | "appearance"
  | "advanced";

export const settingsNav = $state<{ tab: SettingsTab; editCanvas: string | null }>({
  tab: "canvases",
  editCanvas: null,
});

/** Switch sub-nav tab from within the Settings page. */
export function setSettingsTab(tab: SettingsTab): void {
  settingsNav.tab = tab;
  // The edit deep-link only applies to the Canvases tab; drop it elsewhere so
  // returning to Canvases later shows the list, not a stale edit form.
  if (tab !== "canvases") {
    settingsNav.editCanvas = null;
  }
}

/** Navigate to the Settings page on a tab; for "canvases", optionally edit a canvas. */
export function openSettings(tab: SettingsTab = "canvases", editCanvas: string | null = null): void {
  settingsNav.tab = tab;
  settingsNav.editCanvas = editCanvas;
  setPage("settings");
}
