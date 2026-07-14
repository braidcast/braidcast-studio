// Shared opener for the source Filters dialog. Mirrors settingsOpener:
// App owns the single FilterDialog mount gated on `.open`; any component (a source
// context menu, an audio-mixer row) calls openFilters(source, kind) to request it.

export type FilterKind = "audio" | "video" | "all";

export const filterDialogOpener = $state<{ open: boolean; source: string | null; kind: FilterKind }>({
  open: false,
  source: null,
  kind: "all",
});

// Preview suspension is owned by Modal.svelte (FilterDialog wraps it), so the opener
// must NOT suspend too: a second, separately-released suspension raced the modal's on
// close and left the count stuck above zero, so the preview never re-showed.

/** Open the Filters dialog for one source (addressed by name). */
export function openFilters(source: string, kind: FilterKind = "all"): void {
  filterDialogOpener.source = source;
  filterDialogOpener.kind = kind;
  filterDialogOpener.open = true;
}

export function closeFilters(): void {
  filterDialogOpener.open = false;
  filterDialogOpener.source = null;
  filterDialogOpener.kind = "all";
}
