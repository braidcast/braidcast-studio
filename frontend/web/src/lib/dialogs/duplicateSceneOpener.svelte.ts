// Shared opener for the Duplicate Scene dialog. Mirrors transformOpener: App (and
// DetachedApp, since a scene list can be popped out into its own window) owns the
// single DuplicateSceneDialog mount gated on `.scene`; a scene context menu calls
// openDuplicateScene(name, canvas) to request it.

export interface DuplicateSceneRequest {
  /** The scene to duplicate. */
  name: string;
  /** The canvas it lives on; undefined = the Default canvas. */
  canvas?: string;
}

export const duplicateSceneOpener = $state<{ scene: DuplicateSceneRequest | null }>({ scene: null });

/** Open the Duplicate Scene dialog for one scene. */
export function openDuplicateScene(name: string, canvas?: string): void {
  duplicateSceneOpener.scene = { name, canvas };
}

export function closeDuplicateScene(): void {
  duplicateSceneOpener.scene = null;
}
