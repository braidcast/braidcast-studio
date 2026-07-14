// Shared opener for the Go Live "Stream Information" modal. Mirrors
// oauthConnectOpener: App owns the single GoLiveModal mount gated on `.open`; the
// Studio bar requests it with a mode. `golive` confirms by pushing metadata then
// streaming.start; `edit` only pushes (used by the "Edit stream info" button,
// before and mid-stream).

export type GoLiveModalMode = "golive" | "edit";

export const goLiveModal = $state<{ open: boolean; mode: GoLiveModalMode }>({
  open: false,
  mode: "golive",
});

// Preview suspension is owned by Modal.svelte (GoLiveModal wraps it), so the opener
// must NOT suspend too: a second, separately-released suspension raced the modal's on
// close and left the count stuck above zero, so the preview never re-showed.
export function openGoLiveModal(mode: GoLiveModalMode): void {
  goLiveModal.mode = mode;
  goLiveModal.open = true;
}

export function closeGoLiveModal(): void {
  goLiveModal.open = false;
}
