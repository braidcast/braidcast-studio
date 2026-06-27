// Shared opener for the Missing Files relink dialog. Mirrors advAudioOpener:
// App owns the single MissingFilesDialog mount gated on `.open`; the Settings page
// (a source/data tool) calls openMissingFiles() to request it.

import { suspendPreview } from "./previewGate.svelte";

export const missingFilesOpen = $state<{ open: boolean }>({ open: false });

// Hold the preview suspension across the dialog lifetime so the native overlay
// never raises above the modal.
let release: (() => void) | null = null;

/** Open the Missing Files relink dialog. */
export function openMissingFiles(): void {
  missingFilesOpen.open = true;
  release ??= suspendPreview();
}

export function closeMissingFiles(): void {
  missingFilesOpen.open = false;
  release?.();
  release = null;
}
