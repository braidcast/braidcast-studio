// Shared opener for the numeric Edit Transform dialog. Mirrors filterDialogOpener:
// App owns the single TransformDialog mount gated on `.target`; any component (a
// source context menu, the Edit menu) calls openTransform(target, label) to request it.

import { type TransformTarget } from "./bridge";

interface TransformOpenerState {
  target: TransformTarget | null;
  /** Display label for the dialog header (usually the source name). */
  label: string;
}

export const transformOpener = $state<TransformOpenerState>({
  target: null,
  label: "",
});

/** Open the Edit Transform dialog for one scene item. */
export function openTransform(target: TransformTarget, label: string): void {
  transformOpener.target = target;
  transformOpener.label = label;
}

export function closeTransform(): void {
  transformOpener.target = null;
  transformOpener.label = "";
}
