// Detach seam, resolved at CLICK time rather than baked into a panel's params.
//
// The tear-out handler lives in StudioPage (it needs the live DockviewApi to drop
// the torn panel). Threading it through `params.__detach` broke across a layout
// restore: layoutStore serializes params via JSON.stringify, which silently drops
// function values, so every panel rebuilt from layout.json got a dead detach
// button. Instead the main window registers ONE handler here on boot and the
// custom tab calls requestDetach(panelId) at click time — no serialized function,
// so restored panels detach identically to freshly-added ones.
type DetachHandler = (panelId: string) => void;

let handler: DetachHandler | null = null;

export function setDetachHandler(fn: DetachHandler): void {
  handler = fn;
}

export function requestDetach(panelId: string): void {
  handler?.(panelId);
}
