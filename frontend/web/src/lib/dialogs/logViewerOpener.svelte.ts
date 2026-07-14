// Shared opener for the Log Viewer modal. Mirrors missingFilesOpener:
// App owns the single LogViewerDialog mount gated on `.open`; the Settings page
// (a diagnostics tool) calls openLogViewer() to request it.

export const logViewerOpen = $state<{ open: boolean }>({ open: false });

// Preview suspension is owned by Modal.svelte (LogViewerDialog wraps it), so the opener
// must NOT suspend too: a second, separately-released suspension raced the modal's on
// close and left the count stuck above zero, so the preview never re-showed.

/** Open the Log Viewer modal. */
export function openLogViewer(): void {
  logViewerOpen.open = true;
}

export function closeLogViewer(): void {
  logViewerOpen.open = false;
}
