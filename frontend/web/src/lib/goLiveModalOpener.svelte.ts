// Shared opener for the Go Live "Stream Information" modal. Mirrors
// oauthConnectOpener: App owns the single GoLiveModal mount gated on `.open`; the
// Studio bar requests it with a mode. `golive` confirms by pushing metadata then
// streaming.start; `edit` only pushes (used by the "Edit stream info" button,
// before and mid-stream).

import { suspendPreview } from "./previewGate.svelte";

export type GoLiveModalMode = "golive" | "edit";

export const goLiveModal = $state<{ open: boolean; mode: GoLiveModalMode }>({
  open: false,
  mode: "golive",
});

// Hold the native preview suspension across the modal's lifetime so the overlay
// never raises above it (the modal sits over the Studio preview surfaces).
let release: (() => void) | null = null;

export function openGoLiveModal(mode: GoLiveModalMode): void {
  goLiveModal.mode = mode;
  goLiveModal.open = true;
  release ??= suspendPreview();
}

export function closeGoLiveModal(): void {
  goLiveModal.open = false;
  release?.();
  release = null;
}
