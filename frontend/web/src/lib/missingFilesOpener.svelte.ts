// Shared opener for the Missing Files relink dialog. Mirrors advAudioOpener:
// App owns the single MissingFilesDialog mount gated on `.open`; the Settings page
// (a source/data tool) calls openMissingFiles() to request it.

export const missingFilesOpen = $state<{ open: boolean }>({ open: false });

// Preview suspension is owned by Modal.svelte (MissingFilesDialog wraps it), so the opener
// must NOT suspend too: a second, separately-released suspension raced the modal's on
// close and left the count stuck above zero, so the preview never re-showed.

/** Open the Missing Files relink dialog. */
export function openMissingFiles(): void {
  missingFilesOpen.open = true;
}

export function closeMissingFiles(): void {
  missingFilesOpen.open = false;
}
