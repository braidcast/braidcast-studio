import { untrack } from "svelte";
import { SourceSelection, sourceSelection } from "$lib/stores/sourceSelectionStore.svelte";

/** Identifies the dock instance holding the claim. One per component, created at component
 *  scope, so a dock can release only its OWN claim. It cannot be keyed on the
 *  `SourceSelection` instead: the Scenes dock and the Sources dock both drive the shared
 *  `sourceSelection` singleton, so instance identity cannot tell their claims apart and the
 *  Scenes dock — which owns no selection — could never release at all. */
export type SurfaceOwner = symbol;

interface Claim {
  owner: SurfaceOwner | null;
  canvas: string | null;
  selection: SourceSelection;
  kind: "scene" | "source";
  scene: string | null;
  // Answered live by the claiming dock, not snapshotted at click time: the scene count
  // changes under a standing claim (the toolbar's + and Remove claim nothing), and a stale
  // answer either refuses a removal that would succeed or raises a destructive confirm the
  // bridge then refuses.
  canRemoveScene: () => boolean;
}

// No dock has claimed yet (or the last one released): the Default canvas, behaving exactly
// as the app did before this store existed.
const UNCLAIMED: Claim = {
  owner: null,
  canvas: null,
  selection: sourceSelection,
  kind: "source",
  scene: null,
  canRemoveScene: () => false,
};

// Which dock surface the user is working in, for the single window-level keydown handler
// in App.svelte. That handler has no focus context of its own, so without this every
// shortcut resolves against the Default canvas's `sourceSelection` no matter which dock
// was clicked -- Ctrl+C in a CanvasDock copies the Default dock's row, Ctrl+V pastes into
// the Default scene, and Delete removes from it.
//
// `canvas === null` is the Default canvas, the same sentinel defaultCanvasStore and the
// bridge already use (an omitted/empty `canvas` param resolves to the global channel-0
// path in ResolveCanvasTarget); this is not a second sentinel.
//
// The per-list state stays where it already lives -- SourcesDock drives the exported
// `sourceSelection`, each CanvasDock its own instance -- and this singleton only records
// which of those is currently addressed. Same split as DockError in dockError.svelte.ts:
// per-instance state in the dock, one shared pointer at the app level.
class ActiveSurface {
  #owner = $state<SurfaceOwner | null>(null);
  #canvas = $state<string | null>(null);
  #selection = $state<SourceSelection>(sourceSelection);
  #kind = $state<"scene" | "source">("source");
  #claimedScene = $state<string | null>(null);
  #canRemoveScene = $state<() => boolean>(UNCLAIMED.canRemoveScene);

  /** null for the Default canvas; an additional canvas's uuid otherwise. */
  get canvas(): string | null {
    return this.#canvas;
  }

  /** `canvas` shaped for a bridge param: undefined on the Default path, where every
   *  method resolves channel 0 from the omitted param. */
  get canvasParam(): string | undefined {
    return this.#canvas ?? undefined;
  }

  /** The claiming dock's selection model — the target of every item shortcut. */
  get selection(): SourceSelection {
    return this.#selection;
  }

  /** Whether the last claim came from a scene row or a source row. */
  get kind(): "scene" | "source" {
    return this.#kind;
  }

  /** THE scene resolver: every caller that needs "which scene is the user working in"
   *  reads it here, so no two of them can drift onto different answers. A scene claim names
   *  the row that was clicked; otherwise it is the source list's own scene, which trails a
   *  scene click by a bridge round-trip and is null when the Default canvas's Sources dock
   *  is not mounted. */
  get scene(): string | null {
    return this.#kind === "scene" ? this.#claimedScene : this.#selection.scene;
  }

  /** Whether the claiming dock would let its own Remove act right now — the same predicate
   *  that disables its toolbar and context-menu entries, asked of the dock so the keyboard
   *  takes that decision rather than re-deriving one. The bridge still owns the actual
   *  refusal; this only keeps a destructive confirm from being raised for an action that
   *  cannot succeed. False unless a scene claim stands. */
  get canRemoveScene(): boolean {
    return this.#canRemoveScene();
  }

  /** A source row was clicked (or selected from the preview / on creation): this
   *  surface owns the shortcuts and its selection is what they act on. */
  claimSource(owner: SurfaceOwner, canvas: string | null, selection: SourceSelection): void {
    this.#apply({ ...UNCLAIMED, owner, canvas, selection });
  }

  /** A scene row was clicked. The source selection drops whether or not the click
   *  changed the current scene: setCurrent no-ops on an unchanged scene, so nothing
   *  else would clear a selection the user can no longer see, and Delete would remove
   *  it. */
  claimScene(
    owner: SurfaceOwner,
    canvas: string | null,
    selection: SourceSelection,
    scene: string,
    canRemoveScene: () => boolean,
  ): void {
    this.#apply({ owner, canvas, selection, kind: "scene", scene, canRemoveScene });
    selection.clear();
  }

  /** A scene claim names its row by name, so it stops resolving once that name leaves the
   *  list — whether the scene was removed or renamed. A rename through F2 goes through this
   *  store and is not followed: the row loses its row-level claim and the user reselects.
   *  Only the scene identity is dropped; the dock keeps the surface, so the canvas the user
   *  is working in is unchanged. The owning dock calls this whenever its scene list reloads. */
  dropStaleSceneClaim(owner: SurfaceOwner, sceneNames: readonly string[]): void {
    // Reads the claim it also writes, so tracking those reads would make the caller's
    // effect depend on state it just wrote and re-fire without end (the same trap
    // SourceSelection.reconcile documents). untrack the body: the effect then tracks only
    // the scene list it was called with.
    untrack(() => {
      const scene = this.#claimedScene;
      if (this.#owner === owner && this.#kind === "scene" && scene !== null && !sceneNames.includes(scene)) {
        this.#kind = UNCLAIMED.kind;
        this.#claimedScene = UNCLAIMED.scene;
        this.#canRemoveScene = UNCLAIMED.canRemoveScene;
      }
    });
  }

  /** Unmounting docks hand their claim back. Only the dock the store still points at can
   *  release it — another surface may have taken over since. */
  release(owner: SurfaceOwner): void {
    if (this.#owner === owner) {
      this.#apply(UNCLAIMED);
    }
  }

  #apply(c: Claim): void {
    this.#owner = c.owner;
    this.#canvas = c.canvas;
    this.#selection = c.selection;
    this.#kind = c.kind;
    this.#claimedScene = c.scene;
    this.#canRemoveScene = c.canRemoveScene;
  }
}

export const activeSurface = new ActiveSurface();
