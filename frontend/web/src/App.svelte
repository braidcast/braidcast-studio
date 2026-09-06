<script lang="ts">
  import { onMount } from "svelte";
  import TitleBar from "$lib/ui/TitleBar.svelte";
  import NavRail from "$lib/ui/NavRail.svelte";
  import { pageStore } from "$lib/stores/pageStore.svelte";
  import { seamStore, SEAM, railSeam, offsetRight, polygon } from "$lib/stores/seamStore.svelte";
  import StudioPage from "$lib/pages/StudioPage.svelte";
  import CanvasesPage from "$lib/pages/CanvasesPage.svelte";
  import StreamsPage from "$lib/pages/StreamsPage.svelte";
  import OverlaysPage from "$lib/pages/OverlaysPage.svelte";
  import SchedulePage from "$lib/pages/SchedulePage.svelte";
  import MonitorPage from "$lib/pages/MonitorPage.svelte";
  import AiPage from "$lib/pages/AiPage.svelte";
  import SettingsPage from "$lib/pages/SettingsPage.svelte";
  import { themeStore } from "$lib/theme/themeStore.svelte";
  import FilterDialog from "$lib/dialogs/FilterDialog.svelte";
  import { filterDialogOpener, closeFilters } from "$lib/dialogs/filterDialogOpener.svelte";
  import TransformDialog from "$lib/dialogs/TransformDialog.svelte";
  import { transformOpener, closeTransform, openTransform } from "$lib/dialogs/transformOpener.svelte";
  import AdvAudioDialog from "$lib/dialogs/AdvAudioDialog.svelte";
  import { advAudioOpener, closeAdvAudio } from "$lib/dialogs/advAudioOpener.svelte";
  import AboutDialog from "$lib/dialogs/AboutDialog.svelte";
  import { aboutOpen, closeAbout } from "$lib/dialogs/aboutOpener.svelte";
  import MissingFilesDialog from "$lib/dialogs/MissingFilesDialog.svelte";
  import { missingFilesOpen, closeMissingFiles } from "$lib/dialogs/missingFilesOpener.svelte";
  import LogViewerDialog from "$lib/dialogs/LogViewerDialog.svelte";
  import { logViewerOpen, closeLogViewer } from "$lib/dialogs/logViewerOpener.svelte";
  import ImporterDialog from "$lib/dialogs/ImporterDialog.svelte";
  import { importerOpen, closeImporter } from "$lib/dialogs/importerOpener.svelte";
  import OAuthConnectDialog from "$lib/dialogs/OAuthConnectDialog.svelte";
  import { oauthConnect, closeOAuthConnect } from "$lib/dialogs/oauthConnectOpener.svelte";
  import GoLiveModal from "$lib/dialogs/golive/GoLiveModal.svelte";
  import { goLiveModal } from "$lib/dialogs/golive/goLiveModalOpener.svelte";
  import CollectionDialog, { type DialogSpec } from "$lib/dialogs/CollectionDialog.svelte";
  import { planFile, planText, createDropped, type DropPlan } from "$lib/dialogs/add-source/dropSource";
  import { undoStore } from "$lib/stores/undoStore.svelte";
  import { channelsStore } from "$lib/stores/channelsStore.svelte";
  import { diagnosticsStore } from "$lib/stores/diagnosticsStore.svelte";
  import { obs, type SceneItem, type TransformAction, type ReorderDirection, type TransformTarget } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { clipboard } from "$lib/stores/clipboardStore.svelte";
  import { copyItem, pasteReference } from "$lib/stores/clipboardItemState";
  import { activeSurface } from "$lib/stores/activeSurfaceStore.svelte";
  import { dockAction } from "$lib/stores/dockActionSignal.svelte";
  import Toast from "$lib/ui/Toast.svelte";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import { callOrToast } from "$lib/utils/callToast";
  import { isEditable } from "$lib/utils/editableTarget";
  import { previewSuspended } from "$lib/stores/previewGate.svelte";

  // Apply the saved (or default Industrial) theme before first paint settles.
  void themeStore.hydrate();

  // Content notch: the view subtracts a triangle congruent to the rail's bump, offset
  // by a constant perpendicular gap G (offsetRight — a true normal offset, so the gap
  // stays even around the point). The liner is the same notch pulled LINE px back so a
  // --color-border hairline traces the whole seam behind the view. Same vertex count
  // every time so clip-path transitions smoothly. EXTENT covers the view's right/bottom.
  const notchTail: [number, number][] = [
    [SEAM.EXTENT, SEAM.EXTENT],
    [SEAM.EXTENT, 0],
  ];
  const viewClip = $derived(polygon([...offsetRight(railSeam(seamStore.ym), SEAM.G), ...notchTail]));
  const linerClip = $derived(polygon([...offsetRight(railSeam(seamStore.ym), SEAM.G - SEAM.LINE), ...notchTail]));

  // Every shortcut below addresses the dock the user last clicked in, not the Default
  // canvas: `activeSurface` carries that dock's canvas alongside its selection model, so
  // the row and the canvas it is addressed through can never come from two different
  // surfaces. Which canvas and which scene are BOTH resolved by the store (see its `scene`
  // getter, shared with the drop path in dropSource.ts); the helpers below only shape that
  // pair into the bridge's param object, so no call site here can address a row while
  // omitting its canvas.
  function activeParams(): { canvas?: string; scene?: string } {
    return { canvas: activeSurface.canvasParam, scene: activeSurface.scene ?? undefined };
  }

  function activeItem(): { item: SceneItem; target: TransformTarget } | null {
    const item = activeSurface.selection.item;
    return item ? { item, target: { ...activeParams(), id: item.id } } : null;
  }

  // For the shortcuts that address a scene rather than a row (paste, scene rename).
  function activeScene(): { canvas?: string; scene: string } | null {
    const p = activeParams();
    return p.scene ? { canvas: p.canvas, scene: p.scene } : null;
  }

  // The one modal-confirm slot the app-level shortcuts and the drop flow share. An occupied
  // slot is never overwritten: a confirm that mutated into a different question under the
  // user would take an answer meant for the first one, and would discard that one's commit
  // and close handlers unrun. Returns whether the dialog was opened.
  let confirmDialog = $state<DialogSpec | null>(null);
  function openDialog(spec: DialogSpec): boolean {
    if (confirmDialog) {
      return false;
    }
    confirmDialog = spec;
    return true;
  }

  // Delete on a scene row. Removal itself stays in the owning dock — dockAction pokes the
  // same remove() its toolbar and context menu call, so the error surface, the undo entry
  // and the bridge's last-scene refusal are inherited rather than re-derived. Only the
  // confirm is added here: the menu's Remove is a deliberate two-step act, while a single
  // keystroke that destroys a scene and everything in it must ask. The dock's own
  // "can Remove act at all" answer short-circuits ahead of the confirm, so the last scene
  // says so instead of asking a destructive question it would then fail to carry out.
  function removeActiveScene(e: KeyboardEvent): void {
    const s = activeScene();
    // The confirmDialog half is belt-and-braces: Modal suspends the preview while it
    // stands, and the Delete branch is already gated on the preview being live.
    if (!s || confirmDialog) {
      return;
    }
    e.preventDefault();
    if (!activeSurface.canRemoveScene) {
      showToast("Cannot remove the last scene", s.scene, { assertive: true });
      return;
    }
    const canvas = activeSurface.canvas;
    openDialog({
      kind: "confirm",
      title: "Remove Scene",
      message: `Remove "${s.scene}" and everything in it? This can be undone.`,
      confirmLabel: "Remove",
      onCommit: () => void commitRemoveScene(canvas, s.scene),
    });
  }

  // The confirmed removal. The dock that owns the scene performs it; if none took the
  // request — its dock was closed while the confirm stood, or the scene went away under it
  // — say so, because the user answered a destructive question and is owed the outcome.
  async function commitRemoveScene(canvas: string | null, name: string): Promise<void> {
    if (!(await dockAction.request(canvas, { kind: "removeScene", name }))) {
      showToast(`Could not remove "${name}" — it is no longer listed`, name, { assertive: true });
    }
  }

  // Quick transform verbs (reset/fit/stretch/center) all shape the same bridge call
  // against the active surface's selected scene item — mirrors the calls transformMenu.ts's
  // "Transform" submenu makes via sceneItems.transformAction. No-op, no preventDefault,
  // when nothing is selected, so e.g. Ctrl+F still falls through to nothing rather than
  // eating the keystroke.
  function quickTransform(e: KeyboardEvent, action: TransformAction, errPrefix: string): void {
    const t = activeItem();
    if (!t) {
      return;
    }
    e.preventDefault();
    void callOrToast("sceneItems.transformAction", { ...t.target, action }, errPrefix);
  }

  // Arrow-key nudge (mirrors stock OBS: 1px, 10px with Shift). Reads the item's
  // current position and writes back a position-only patch through the same
  // get/setTransform pair quickTransform/copy-paste-transform use above — position
  // is in canvas pixels, y growing downward, matching the preview and the Edit
  // Transform dialog.
  async function nudge(dx: number, dy: number): Promise<void> {
    const t = activeItem();
    if (!t) {
      return;
    }
    const params = t.target;
    const xf = await callOrToast("sceneItems.getTransform", params, "Nudge failed");
    if (!xf) {
      return;
    }
    void callOrToast(
      "sceneItems.setTransform",
      { ...params, transform: { pos: { x: xf.pos.x + dx, y: xf.pos.y + dy } } },
      "Nudge failed",
    );
  }

  const ARROW_DELTAS: Record<string, [number, number]> = {
    ArrowLeft: [-1, 0],
    ArrowRight: [1, 0],
    ArrowUp: [0, -1],
    ArrowDown: [0, 1],
  };

  // Ctrl+Arrow / Ctrl+Home / Ctrl+End reorder the selected item; "up" moves toward the
  // topmost row (index 0), matching the SourcesDock up/down buttons and the top-first list.
  const ORDER_DIRECTIONS: Record<string, ReorderDirection> = {
    ArrowUp: "up",
    ArrowDown: "down",
    Home: "top",
    End: "bottom",
  };

  function onKeydown(e: KeyboardEvent): void {
    // Fullscreen toggles regardless of focus (not gated on editable target).
    if (e.key === "F11") {
      e.preventDefault();
      void obs.call("window.toggleFullscreen").catch(() => {});
      return;
    }
    // Arrow-key nudge of the active surface's selected scene item on its preview. Leaves
    // Ctrl/Alt+Arrow, editable targets, and modal-open (arrows belong to the
    // dialog) alone; no-ops (without preventDefault) when nothing is selected so
    // e.g. list navigation still gets the arrow.
    const arrowDelta = ARROW_DELTAS[e.key];
    if (arrowDelta && !e.ctrlKey && !e.altKey && !isEditable(e.target) && !previewSuspended()) {
      if (activeSurface.selection.item) {
        e.preventDefault();
        const step = e.shiftKey ? 10 : 1;
        void nudge(arrowDelta[0] * step, arrowDelta[1] * step);
      }
      return;
    }
    // Delete removes whatever the active surface last claimed — a scene row or the selected
    // scene item(s) — via the same bridge path that surface's own "Remove" uses (server-side
    // undoable). Gated like the nudge above, and the !isEditable gate wraps BOTH branches so
    // an in-flight inline rename keeps the key for its text. No-op (no preventDefault) when
    // there is nothing to remove, so the key falls through.
    if (e.key === "Delete" && !e.ctrlKey && !e.altKey && !isEditable(e.target) && !previewSuspended()) {
      if (activeSurface.kind === "scene") {
        removeActiveScene(e);
        return;
      }
      // Batch remove: one existing single-item remove per selected id (each independently
      // undoable). Snapshot the ids — the removals shrink the set mid-loop. A single
      // selection removes exactly one, unchanged from before.
      const params = activeParams();
      const ids = [...activeSurface.selection.ids];
      if (ids.length > 0) {
        e.preventDefault();
        for (const id of ids) {
          void callOrToast("sceneItems.remove", { ...params, id }, "Remove failed");
        }
      }
      return;
    }
    // F2 starts inline rename of the current selection, reusing the docks' existing
    // beginRename via dockAction (no rename editor/bridge here). A selected source item
    // wins; otherwise the active surface's scene. Gated like the nudge/Delete siblings;
    // no-op (no preventDefault) when nothing is selected so the key falls through.
    if (e.key === "F2" && !e.ctrlKey && !e.altKey && !isEditable(e.target) && !previewSuspended()) {
      // The surface's kind, not merely "is a row selected", decides which editor opens:
      // a scene-row click is a scene target even if a row somehow stayed selected.
      const t = activeSurface.kind === "source" ? activeItem() : null;
      const s = t ? null : activeScene();
      if (t) {
        e.preventDefault();
        void dockAction.request(activeSurface.canvas, { kind: "renameSource", id: t.item.id });
      } else if (s) {
        e.preventDefault();
        void dockAction.request(activeSurface.canvas, { kind: "renameScene", name: s.scene });
      }
      return;
    }
    // Windows modifier; ignore Alt-combos and editable targets.
    if (!e.ctrlKey || e.altKey || isEditable(e.target)) {
      return;
    }
    // Ctrl+Arrow / Ctrl+Home / Ctrl+End reorder the selected item, reusing the same
    // direction-based sceneItems.reorder the SourcesDock buttons call; the backend clamps
    // at the ends. No-op (no preventDefault) when nothing is selected.
    const orderDir = ORDER_DIRECTIONS[e.key];
    if (orderDir) {
      const t = activeItem();
      if (t) {
        e.preventDefault();
        void callOrToast("sceneItems.reorder", { ...t.target, direction: orderDir }, "Reorder failed");
      }
      return;
    }
    const key = e.key.toLowerCase();
    if (key === "z" && !e.shiftKey) {
      e.preventDefault();
      undoStore.undo();
    } else if ((key === "z" && e.shiftKey) || key === "y") {
      e.preventDefault();
      undoStore.redo();
    } else if (key === "c" && !e.shiftKey) {
      // Copy the active surface's selected source, carrying its full item state (§1.7).
      const t = activeItem();
      if (t?.item.source) {
        e.preventDefault();
        void copyItem(t.target, t.item);
      }
    } else if (key === "v" && !e.shiftKey) {
      // Paste a reference of the copied source into the active surface's current scene,
      // then re-apply the carried item state (transform/blend/color/visibility/scale).
      const s = activeScene();
      if (clipboard.source && s) {
        e.preventDefault();
        void pasteReference(s).catch((err) => showToast("Paste failed: " + (err as Error).message, "paste"));
      }
    } else if (key === "c" && e.shiftKey) {
      // Copy the transform of the active surface's selected scene item (mirrors the
      // "Copy Transform" context-menu action / clipboard.transform in the docks).
      const t = activeItem();
      if (t) {
        e.preventDefault();
        void callOrToast("sceneItems.getTransform", t.target, "Copy transform failed").then((xf) => {
          if (xf) {
            clipboard.transform = xf;
          }
        });
      }
    } else if (key === "v" && e.shiftKey) {
      // Paste the copied transform onto the active surface's selected scene item.
      const t = activeItem();
      if (t && clipboard.transform) {
        e.preventDefault();
        void callOrToast(
          "sceneItems.setTransform",
          { ...t.target, transform: clipboard.transform },
          "Paste transform failed",
        );
      }
    } else if (key === "s" && e.shiftKey) {
      // Ctrl+Shift+S: screenshot the program (Default canvas). OBS leaves its
      // screenshot hotkey unbound by default, so this is our own clear default.
      e.preventDefault();
      void callOrToast("screenshot.takeProgram", undefined, "Screenshot failed");
    } else if (key === "s" && !e.shiftKey) {
      quickTransform(e, "stretchToScreen", "Stretch to screen failed");
    } else if (key === "e") {
      // Edit transform: open the same numeric dialog the context menu's
      // "Edit Transform" item opens.
      const t = activeItem();
      if (t) {
        e.preventDefault();
        openTransform(t.target, t.item.source ?? "(unnamed)");
      }
    } else if (key === "r") {
      quickTransform(e, "reset", "Reset transform failed");
    } else if (key === "f") {
      quickTransform(e, "fitToScreen", "Fit to screen failed");
    } else if (key === "d") {
      quickTransform(e, "center", "Center transform failed");
    }
  }

  // Ctrl+wheel (and trackpad pinch, which arrives as ctrl+wheel) zooms the whole page
  // in a browser; a desktop app must not. The keyboard zoom keys are killed natively
  // in the CEF client; this covers the wheel path, which a key handler can't see.
  function onWheel(e: WheelEvent): void {
    if (e.ctrlKey) e.preventDefault();
  }

  // A file/text/link dropped anywhere on the window makes CEF navigate to it, which
  // blows away the SPA. Cancel the default on the window so a stray drop never
  // navigates; real drop targets still receive their own (bubbling) drop event.
  function onDragOver(e: DragEvent): void {
    e.preventDefault();
  }

  // A drag that never touches our own document (Explorer, another window) can only
  // ever start with a dragstart we never saw; one that did start here — a reorder
  // drag or a plain text selection — always fires dragstart on this window first.
  // That makes "did dragstart fire in our document" a complete test for drags that
  // start in this document; content dragged out of a hosted iframe isn't covered.
  let internalDrag = false;

  // §1.6 parity: a drop becomes a source. Files -> image/media, a URL/.html ->
  // browser (after a confirm, through the shared confirmDialog slot above), other text ->
  // a text source. dataTransfer is only live during dispatch, so read every field
  // synchronously before any await.

  // A file input and an editable field both handle a drop themselves. Cancelling the
  // default here would leave the field empty AND turn the drop into a source, so those
  // targets are left entirely alone -- including the preventDefault.
  function handlesItsOwnDrop(e: DragEvent): boolean {
    const el = e.target as HTMLElement | null;
    if (!el?.closest) return false;
    const hasFiles = (e.dataTransfer?.files?.length ?? 0) > 0;
    if (el.closest('input[type="file"]')) return hasFiles;
    return !hasFiles && !!el.closest("input, textarea, [contenteditable='true']");
  }

  async function onWindowDrop(e: DragEvent): Promise<void> {
    if (handlesItsOwnDrop(e)) {
      internalDrag = false;
      return;
    }
    e.preventDefault();
    // Read and cleared synchronously before any await, so a drag whose dragend never
    // arrives at all costs one swallowed drop rather than latching for the session.
    const wasInternal = internalDrag;
    internalDrag = false;
    if (wasInternal) return;
    const dt = e.dataTransfer;
    if (!dt) return;
    // Files take precedence over the synthetic uri-list/plain text a file drag also
    // carries. planFile returns null when this build hides File.path (see dropSource).
    const files = Array.from(dt.files) as (File & { path?: string })[];
    if (files.length > 0) {
      for (const file of files) {
        const plan = planFile(file);
        if (plan) await runDrop(plan);
      }
      return;
    }
    // A dropped hyperlink carries the URL in uri-list; take its first non-comment line.
    const uri = dt.getData("text/uri-list");
    const line = uri ? uri.split("\n").find((s) => s.trim() && !s.startsWith("#")) : "";
    const text = (line || dt.getData("text/plain")).trim();
    if (!text) return;
    const plan = await planText(text);
    if (plan) await runDrop(plan);
  }

  async function runDrop(plan: DropPlan): Promise<void> {
    if (plan.confirm) {
      const opened = openDialog({
        kind: "confirm",
        title: "Add Source",
        message: plan.confirm,
        confirmLabel: "Create",
        onCommit: () => void createDroppedSource(plan),
      });
      if (!opened) {
        showToast("Finish the open dialog before adding this source", plan.name, { assertive: true });
      }
      return;
    }
    await createDroppedSource(plan);
  }

  async function createDroppedSource(plan: DropPlan): Promise<void> {
    try {
      const name = await createDropped(plan);
      showToast("Added source: " + name, name);
    } catch (e) {
      const msg = (e as Error).message;
      showToast("Could not add source: " + msg, msg);
    }
  }

  onMount(() => {
    undoStore.start();
    // Seed the DEBUG gate + log path early so log.dbg is gated correctly app-wide.
    diagnosticsStore.start();
    const offChannels = channelsStore.init();
    // Kill browser spellcheck squiggles app-wide (inherited); real prose fields can
    // still opt back in with spellcheck="true".
    document.body.spellcheck = false;
    window.addEventListener("keydown", onKeydown);
    window.addEventListener("wheel", onWheel, { passive: false });
    window.addEventListener("dragover", onDragOver);
    window.addEventListener("drop", onWindowDrop);
    // dragend always fires at the source node, so the clear rides on that node rather
    // than on a window listener: a node detached mid-drag — a keyed list re-rendering
    // under the drag — no longer has a path to window, and the flag would latch.
    const onDragStartCapture = (e: DragEvent) => {
      internalDrag = true;
      e.target?.addEventListener("dragend", () => (internalDrag = false), { once: true });
      // Capture runs before the handlers that may cancel the drag, and a cancelled
      // dragstart begins no session -- so no dragend and no drop ever follow to clear
      // the flag. Re-check once dispatch has finished.
      queueMicrotask(() => {
        if (e.defaultPrevented) internalDrag = false;
      });
    };
    window.addEventListener("dragstart", onDragStartCapture, true);
    // Surface every saved screenshot (program or source) as a transient toast.
    const offShot = obs.on(EV.screenshotSaved, (p) => {
      const file = p.path.split(/[\\/]/).pop() || p.path;
      showToast("Screenshot saved: " + file, p.path);
    });
    return () => {
      window.removeEventListener("keydown", onKeydown);
      window.removeEventListener("wheel", onWheel);
      window.removeEventListener("dragover", onDragOver);
      window.removeEventListener("drop", onWindowDrop);
      window.removeEventListener("dragstart", onDragStartCapture, true);
      offShot();
      offChannels();
    };
  });
</script>

<div class="app-root">
  <TitleBar />
  <div class="shell">
    <NavRail />
    <div class="seam-liner" style="left:{SEAM.W}px; clip-path:{linerClip}"></div>
    <main class="view" style="clip-path:{viewClip}">
    <!-- Studio stays permanently mounted (hidden, not unmounted, off-page) so the
         Dockview workspace + reconciler keep their single onReady lifecycle exactly
         as before — switching pages must not tear down or rebuild the docks. -->
    <StudioPage />
    {#if pageStore.page === "canvases"}
      <CanvasesPage />
    {:else if pageStore.page === "streams"}
      <StreamsPage />
    {:else if pageStore.page === "overlays"}
      <OverlaysPage />
    {:else if pageStore.page === "schedule"}
      <SchedulePage />
    {:else if pageStore.page === "monitor"}
      <MonitorPage />
    {:else if pageStore.page === "ai"}
      <AiPage />
    {:else if pageStore.page === "settings"}
      <SettingsPage />
    {/if}
    </main>
  </div>
</div>

{#if filterDialogOpener.open && filterDialogOpener.source}
  <FilterDialog source={filterDialogOpener.source} onClose={closeFilters} />
{/if}

{#if transformOpener.target}
  <TransformDialog target={transformOpener.target} label={transformOpener.label} onClose={closeTransform} />
{/if}

{#if advAudioOpener.open && advAudioOpener.source}
  <AdvAudioDialog source={advAudioOpener.source} label={advAudioOpener.label} onClose={closeAdvAudio} />
{/if}

{#if aboutOpen.open}
  <AboutDialog onClose={closeAbout} />
{/if}

{#if missingFilesOpen.open}
  <MissingFilesDialog onClose={closeMissingFiles} />
{/if}

{#if logViewerOpen.open}
  <LogViewerDialog onClose={closeLogViewer} />
{/if}

{#if importerOpen.open}
  <ImporterDialog onClose={closeImporter} />
{/if}

{#if oauthConnect.open && oauthConnect.req}
  <OAuthConnectDialog req={oauthConnect.req} onClose={closeOAuthConnect} />
{/if}

{#if goLiveModal.open}
  <GoLiveModal />
{/if}

{#if confirmDialog}
  <CollectionDialog {...confirmDialog} onClose={() => (confirmDialog = null)} />
{/if}

<Toast />

<style>
  /* Column shell: custom title bar on top, the app body fills the rest. Clips at the
     root so the document never scrolls (overflow lives inside the panes/pages). */
  .app-root {
    display: flex;
    flex-direction: column;
    height: 100%;
    overflow: hidden;
  }
  .shell {
    position: relative;
    z-index: 0;
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: row;
    overflow: hidden;
    /* Base shows through the constant-width seam channel where both the view and the
       liner are clipped away. z-index:0 makes .shell the stacking context so the liner
       (z:-1) paints above this background but below the in-flow view content. */
    background: var(--color-base);
  }
  /* Seam hairline: the notch pulled LINE px toward the rail, in --color-border. The
     view clip reveals a LINE-px strip of it along the whole edge (straight + triangle),
     so a base channel + border hairline flank the seam instead of a bare gap. */
  .seam-liner {
    position: absolute;
    top: 0;
    right: 0;
    bottom: 0;
    z-index: -1;
    background: var(--color-border);
    pointer-events: none;
  }
  .view {
    flex: 1;
    min-width: 0;
    min-height: 0;
    display: flex;
    flex-direction: column;
    /* Left edge at the rail's straight edge (abs 70); the clip geometry — not a margin
       — now owns the gap. clip-path is authored in this frame (view-local x = frame x). */
    will-change: clip-path;
  }
  @media (prefers-reduced-motion: no-preference) {
    .seam-liner,
    .view {
      transition: clip-path 0.28s cubic-bezier(0.4, 0, 0.15, 1);
    }
  }
</style>
