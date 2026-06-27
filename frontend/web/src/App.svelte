<script lang="ts">
  import { onMount } from "svelte";
  import NavRail from "./lib/NavRail.svelte";
  import { pageStore } from "./lib/pageStore.svelte";
  import StudioPage from "./lib/pages/StudioPage.svelte";
  import StreamPage from "./lib/pages/StreamPage.svelte";
  import SchedulePage from "./lib/pages/SchedulePage.svelte";
  import MonitorPage from "./lib/pages/MonitorPage.svelte";
  import AiPage from "./lib/pages/AiPage.svelte";
  import SettingsPage from "./lib/pages/SettingsPage.svelte";
  import { themeStore } from "./lib/theme/themeStore.svelte";
  import FilterDialog from "./lib/FilterDialog.svelte";
  import { filterDialogOpener, closeFilters } from "./lib/filterDialogOpener.svelte";
  import TransformDialog from "./lib/TransformDialog.svelte";
  import { transformOpener, closeTransform } from "./lib/transformOpener.svelte";
  import AdvAudioDialog from "./lib/AdvAudioDialog.svelte";
  import { advAudioOpener, closeAdvAudio } from "./lib/advAudioOpener.svelte";
  import AboutDialog from "./lib/AboutDialog.svelte";
  import { aboutOpen, closeAbout } from "./lib/aboutOpener.svelte";
  import MissingFilesDialog from "./lib/MissingFilesDialog.svelte";
  import { missingFilesOpen, closeMissingFiles } from "./lib/missingFilesOpener.svelte";
  import { undoStore } from "./lib/undoStore.svelte";
  import { obs } from "./lib/bridge";
  import { clipboard } from "./lib/clipboardStore.svelte";
  import { sourceSelection } from "./lib/sourceSelectionStore.svelte";

  // Apply the saved (or default Industrial) theme before first paint settles.
  void themeStore.hydrate();

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
        obs.call("sources.addExisting", { scene: sourceSelection.scene, name: clipboard.source.ref }).catch(() => {});
      }
    }
  }

  onMount(() => {
    undoStore.start();
    window.addEventListener("keydown", onKeydown);
    return () => window.removeEventListener("keydown", onKeydown);
  });
</script>

<div class="shell">
  <NavRail />
  <main class="view">
    <!-- Studio stays permanently mounted (hidden, not unmounted, off-page) so the
         Dockview workspace + reconciler keep their single onReady lifecycle exactly
         as before — switching pages must not tear down or rebuild the docks. -->
    <StudioPage />
    {#if pageStore.page === "stream"}
      <StreamPage />
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

<style>
  .shell {
    display: flex;
    flex-direction: row;
    height: 100%;
    background: var(--color-base);
  }
  .view {
    flex: 1;
    min-width: 0;
    min-height: 0;
    display: flex;
    flex-direction: column;
  }
</style>
