// The "focused" canvas — the one whose scene the shared Sources panel edits.
//
// Each CanvasPanel sets this when clicked/interacted with; the shared
// SourcesPanel reads `focusedCanvas` to resolve which canvas's current scene it
// lists + mutates. The Default canvas (or first enabled) is the initial focus;
// CanvasPanels keeps it valid as panels appear/disappear.

export const canvasFocus = $state<{ uuid: string | null }>({ uuid: null });

export function setFocusedCanvas(uuid: string): void {
  canvasFocus.uuid = uuid;
}
