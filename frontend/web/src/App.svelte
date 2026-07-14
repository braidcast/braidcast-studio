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
  import { transformOpener, closeTransform } from "$lib/dialogs/transformOpener.svelte";
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
  import { undoStore } from "$lib/stores/undoStore.svelte";
  import { channelsStore } from "$lib/stores/channelsStore.svelte";
  import { diagnosticsStore } from "$lib/stores/diagnosticsStore.svelte";
  import { obs } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { clipboard } from "$lib/stores/clipboardStore.svelte";
  import { sourceSelection } from "$lib/stores/sourceSelectionStore.svelte";
  import Toast from "$lib/ui/Toast.svelte";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import { callOrToast } from "$lib/utils/callToast";

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

  // Skip the global undo/redo shortcut when the focus is in a text-editing field so
  // native per-field undo (and rename/search typing) keeps working.
  function isEditable(t: EventTarget | null): boolean {
    if (!(t instanceof HTMLElement)) {
      return false;
    }
    const tag = t.tagName;
    return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT" || t.isContentEditable;
  }

  function onKeydown(e: KeyboardEvent): void {
    // Fullscreen toggles regardless of focus (not gated on editable target).
    if (e.key === "F11") {
      e.preventDefault();
      void obs.call("window.toggleFullscreen").catch(() => {});
      return;
    }
    // Windows modifier; ignore Alt-combos and editable targets.
    if (!e.ctrlKey || e.altKey || isEditable(e.target)) {
      return;
    }
    const key = e.key.toLowerCase();
    if (key === "z" && !e.shiftKey) {
      e.preventDefault();
      undoStore.undo();
    } else if ((key === "z" && e.shiftKey) || key === "y") {
      e.preventDefault();
      undoStore.redo();
    } else if (key === "c") {
      // Copy the globally-selected source as a reference (name) for later paste.
      const it = sourceSelection.item;
      if (it?.source) {
        e.preventDefault();
        clipboard.source = { ref: it.source, name: it.source };
      }
    } else if (key === "v") {
      // Paste a reference of the copied source into the global current scene.
      if (clipboard.source && sourceSelection.scene) {
        e.preventDefault();
        void callOrToast(
          "sources.addExisting",
          { scene: sourceSelection.scene, name: clipboard.source.ref },
          "Paste failed",
        );
      }
    } else if (key === "s" && e.shiftKey) {
      // Ctrl+Shift+S: screenshot the program (Default canvas). OBS leaves its
      // screenshot hotkey unbound by default, so this is our own clear default.
      e.preventDefault();
      void obs.call("screenshot.takeProgram").catch(() => {});
    }
  }

  // Ctrl+wheel (and trackpad pinch, which arrives as ctrl+wheel) zooms the whole page
  // in a browser; a desktop app must not. The keyboard zoom keys are killed natively
  // in the CEF client; this covers the wheel path, which a key handler can't see.
  function onWheel(e: WheelEvent): void {
    if (e.ctrlKey) e.preventDefault();
  }

  // A file/text/link dropped anywhere on the window makes CEF navigate to it, which
  // blows away the SPA. Cancel the default on the window so a stray drop is inert;
  // real drop targets still receive their own (bubbling) drop event to read the data.
  function onDragOver(e: DragEvent): void {
    e.preventDefault();
  }
  function onWindowDrop(e: DragEvent): void {
    e.preventDefault();
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
