// Shared opener for the Settings modal, so any component can request it on a
// specific tab (and, for the Canvases tab, with one canvas pre-opened for edit).
// TopBar owns the single SettingsModal mount and reads this store.

export type SettingsTab = "video" | "audio" | "canvases" | "streams" | "outputs";

export const settingsOpener = $state<{
  open: boolean;
  tab: SettingsTab;
  editCanvas: string | null;
}>({ open: false, tab: "video", editCanvas: null });

/** Open Settings on a tab; for "canvases", optionally edit a specific canvas. */
export function openSettings(tab: SettingsTab = "video", editCanvas: string | null = null): void {
  settingsOpener.tab = tab;
  settingsOpener.editCanvas = editCanvas;
  settingsOpener.open = true;
}

export function closeSettings(): void {
  settingsOpener.open = false;
  settingsOpener.editCanvas = null;
}
