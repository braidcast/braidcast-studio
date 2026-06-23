// Shared opener for the source Filters dialog. Mirrors settingsOpener/themeEditorOpener:
// App owns the single FilterDialog mount gated on `.open`; any component (a source
// context menu, an audio-mixer row) calls openFilters(source) to request it.

export const filterDialogOpener = $state<{ open: boolean; source: string | null }>({
  open: false,
  source: null,
});

/** Open the Filters dialog for one source (addressed by name). */
export function openFilters(source: string): void {
  filterDialogOpener.source = source;
  filterDialogOpener.open = true;
}

export function closeFilters(): void {
  filterDialogOpener.open = false;
  filterDialogOpener.source = null;
}
