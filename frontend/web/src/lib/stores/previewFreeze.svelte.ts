// A still of the canvas, held in the DOM while the native preview surface is hidden.
//
// The preview is a child HWND the OS composites above the whole CEF window, so a menu
// or a modal that overlaps it can only be shown by hiding the preview -- which is why
// right-clicking the canvas blanked it. This keeps the last frame on screen in the
// surface's place for as long as the overlay is up, so the preview reads as paused
// rather than broken.
//
// Explicitly a stand-in, not the fix. The frame does not advance, and anything that
// wants live video under a modal needs the boundary itself to go away; the options and
// the ruling against accommodation designs are in
// braidcast-notes/preview-architecture.md.

import { obs } from "$lib/api/bridge";

export class PreviewFreeze {
  /** PNG data URI of the held frame, or null when the surface is live. */
  frame = $state<string | null>(null);

  // Per-capture token. A capture is async, so an overlay closed while one is in
  // flight must not have its own result painted over the live surface afterwards;
  // clear() bumps this and the stale resolution is dropped.
  #seq = 0;

  /**
   * Grab the current frame. Call BEFORE hiding the surface: both are bridge calls and
   * run in order on the host's UI thread, so capturing second would read a canvas
   * whose composite has already been gated off.
   *
   * `canvasUuid` omitted addresses the Default canvas, the same convention every other
   * preview method uses. A failure is not surfaced -- there is no still, the surface
   * is simply blank as it was before, and an error toast on right-click would be worse
   * than the thing it reports.
   */
  async capture(canvasUuid?: string): Promise<void> {
    const seq = ++this.#seq;
    try {
      const r = await obs.call("preview.freeze", canvasUuid ? { canvas: canvasUuid } : {});
      if (seq === this.#seq) {
        this.frame = r.dataUri;
      }
    } catch {
      // No still. The surface reads as it did before this existed.
    }
  }

  /** Drop the still; the live surface is coming back. */
  clear(): void {
    this.#seq++;
    this.frame = null;
  }
}
