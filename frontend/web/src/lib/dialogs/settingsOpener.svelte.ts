// Shared navigation state for the Settings page (7.3). The Settings UI is a full
// nav-rail page, not a modal. SettingsPage reads settingsNav; the sub-nav buttons
// call setSettingsTab. (Canvases + Stream Profiles moved out to their own nav-rail
// pages.)

export type SettingsTab = "general" | "audio" | "hotkeys" | "browserDocks" | "appearance" | "advanced" | "diagnostics";

export const settingsNav = $state<{ tab: SettingsTab }>({
  tab: "general",
});

/** Switch sub-nav tab from within the Settings page. */
export function setSettingsTab(tab: SettingsTab): void {
  settingsNav.tab = tab;
}
