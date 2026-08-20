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

// Binding uuids armed since this modal opened whose stream info has not been validated
// yet. Held HERE and not in the modal because the destinations-panel toggle arms the
// binding BEFORE the modal mounts, and the modal is what has to switch it back off if
// the user leaves without confirming it. A plain Set, not reactive state: only the modal
// reads it -- its primary, to know which arms are its own, and its close, to put the rest
// back -- so nothing renders from it.
export const goLiveArmedHere = new Set<string>();

// Preview suspension is owned by Modal.svelte (GoLiveModal wraps it), so the opener
// must NOT suspend too: a second, separately-released suspension raced the modal's on
// close and left the count stuck above zero, so the preview never re-showed.
export function openGoLiveModal(mode: GoLiveModalMode, armedHere: string[] = []): void {
  // ADDITIVE, never a fresh set. The panel arms and then opens, and until the modal
  // actually mounts there is no backdrop over the panel -- so a second arm can arrive
  // before the first modal exists, and clearing here would discard the first seed and
  // leave that destination armed with nothing tracking it. The modal owns the reset
  // instead: it clears on close and again when it unmounts, so no set outlives one.
  for (const uuid of armedHere) {
    goLiveArmedHere.add(uuid);
  }
  goLiveModal.mode = mode;
  goLiveModal.open = true;
}

export function closeGoLiveModal(): void {
  goLiveModal.open = false;
}
