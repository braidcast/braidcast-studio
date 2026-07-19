import type { CopiedFilter, ItemTransition, Transform } from "$lib/api/bridge";

// The full visual state copied alongside a source reference (§1.7 parity) so a
// paste reproduces the original item's look, not just a bare reference. Matches the
// fields Qt's SourceCopyInfo carried; each has a setter in the applied set (see
// clipboardItemState.ts). show/hideTransition are null when the item has none.
export interface CopiedItemState {
  transform: Transform;
  blendMode: string;
  blendMethod: string;
  scaleFilter: string;
  color: string;
  visible: boolean;
  showTransition: ItemTransition | null;
  hideTransition: ItemTransition | null;
}

// The source clipboard entry. `ref` is the source NAME, which sources.addExisting
// consumes (it resolves by obs_get_source_by_name) for a reference paste. `origin`
// locates the copied scene item so Paste (Duplicate) can hand it to the item-id-
// based sources.duplicate; `state` is the captured look applied after either paste.
// Both are optional so a bare/legacy copy still pastes as a plain reference.
export interface CopiedSource {
  ref: string;
  name: string;
  origin?: { canvas?: string; scene?: string | null; id: number };
  state?: CopiedItemState;
}

// Editor clipboard for copy/paste of sources, filter chains, and transforms.
class ClipboardStore {
  source = $state<CopiedSource | null>(null);
  filters = $state<CopiedFilter[] | null>(null);
  transform = $state<Transform | null>(null);
}

export const clipboard = new ClipboardStore();
