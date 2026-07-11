// Shared opener for the OBS Studio importer wizard. Mirrors logViewerOpener:
// App owns the single ImporterDialog mount gated on `.open`; the Settings page
// (an import tool) calls openImporter() to request it.

export const importerOpen = $state<{ open: boolean }>({ open: false });

// Preview suspension is owned by Modal.svelte (ImporterDialog wraps it), so the opener
// must NOT suspend too: a second, separately-released suspension raced the modal's on
// close and left the count stuck above zero, so the preview never re-showed.

/** Open the OBS Studio importer wizard. */
export function openImporter(): void {
  importerOpen.open = true;
}

export function closeImporter(): void {
  importerOpen.open = false;
}
