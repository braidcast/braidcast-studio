// Shared opener for the Advanced Audio Properties dialog. Mirrors filterDialogOpener:
// App owns the single AdvAudioDialog mount gated on `.open`; any component (an
// audio-mixer row) calls openAdvAudio(source, label) to request it.

import { suspendPreview } from "./previewGate.svelte";

export const advAudioOpener = $state<{ open: boolean; source: string | null; label: string }>({
  open: false,
  source: null,
  label: "",
});

// Hold the preview suspension across the whole dialog lifetime (acquired
// synchronously at open, released on close) so the native overlay never re-raises
// above the modal during a handoff.
let release: (() => void) | null = null;

/** Open the Advanced Audio dialog for one source (addressed by name or uuid). */
export function openAdvAudio(source: string, label: string): void {
  advAudioOpener.source = source;
  advAudioOpener.label = label;
  advAudioOpener.open = true;
  release ??= suspendPreview();
}

export function closeAdvAudio(): void {
  advAudioOpener.open = false;
  advAudioOpener.source = null;
  advAudioOpener.label = "";
  release?.();
  release = null;
}
