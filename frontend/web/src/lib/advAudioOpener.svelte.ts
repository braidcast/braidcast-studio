// Shared opener for the Advanced Audio Properties dialog. Mirrors filterDialogOpener:
// App owns the single AdvAudioDialog mount gated on `.open`; any component (an
// audio-mixer row) calls openAdvAudio(source, label) to request it.

export const advAudioOpener = $state<{ open: boolean; source: string | null; label: string }>({
  open: false,
  source: null,
  label: "",
});

// Preview suspension is owned by Modal.svelte (AdvAudioDialog wraps it), so the
// opener must NOT suspend too: a second, separately-released suspension raced the
// modal's on close and left the count stuck above zero, so the preview never re-showed.

/** Open the Advanced Audio dialog for one source (addressed by name or uuid). */
export function openAdvAudio(source: string, label: string): void {
  advAudioOpener.source = source;
  advAudioOpener.label = label;
  advAudioOpener.open = true;
}

export function closeAdvAudio(): void {
  advAudioOpener.open = false;
  advAudioOpener.source = null;
  advAudioOpener.label = "";
}
