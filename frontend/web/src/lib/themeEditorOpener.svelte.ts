// Shared opener for the Theme Editor modal. Mirrors settingsOpener: App owns the
// single ThemeEditor mount gated on `.open`; any component (the Docks menu) calls
// openThemeEditor() to request it.

export const themeEditorOpener = $state<{ open: boolean }>({ open: false });

export function openThemeEditor(): void {
  themeEditorOpener.open = true;
}

export function closeThemeEditor(): void {
  themeEditorOpener.open = false;
}
