<script lang="ts">
  import NavRail from "./lib/NavRail.svelte";
  import { pageStore } from "./lib/pageStore.svelte";
  import StudioPage from "./lib/pages/StudioPage.svelte";
  import StreamPage from "./lib/pages/StreamPage.svelte";
  import SchedulePage from "./lib/pages/SchedulePage.svelte";
  import MonitorPage from "./lib/pages/MonitorPage.svelte";
  import AiPage from "./lib/pages/AiPage.svelte";
  import SettingsPage from "./lib/pages/SettingsPage.svelte";
  import { themeStore } from "./lib/theme/themeStore.svelte";
  import SettingsModal from "./lib/SettingsModal.svelte";
  import { settingsOpener, closeSettings } from "./lib/settingsOpener.svelte";
  import ThemeEditor from "./lib/ThemeEditor.svelte";
  import { themeEditorOpener, closeThemeEditor } from "./lib/themeEditorOpener.svelte";
  import FilterDialog from "./lib/FilterDialog.svelte";
  import { filterDialogOpener, closeFilters } from "./lib/filterDialogOpener.svelte";
  import TransformDialog from "./lib/TransformDialog.svelte";
  import { transformOpener, closeTransform, startDefaultSelectionTracking } from "./lib/transformOpener.svelte";
  import AboutDialog from "./lib/AboutDialog.svelte";
  import { aboutOpen, closeAbout } from "./lib/aboutOpener.svelte";

  // Apply the saved (or default Industrial) theme before first paint settles.
  void themeStore.hydrate();

  // Track the default-canvas selection so the Edit > Transform menu item enables.
  startDefaultSelectionTracking();
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

{#if settingsOpener.open}
  <SettingsModal initialTab={settingsOpener.tab} editCanvas={settingsOpener.editCanvas} onClose={closeSettings} />
{/if}

{#if themeEditorOpener.open}
  <ThemeEditor onClose={closeThemeEditor} />
{/if}

{#if filterDialogOpener.open && filterDialogOpener.source}
  <FilterDialog source={filterDialogOpener.source} onClose={closeFilters} />
{/if}

{#if transformOpener.target}
  <TransformDialog target={transformOpener.target} label={transformOpener.label} onClose={closeTransform} />
{/if}

{#if aboutOpen.open}
  <AboutDialog onClose={closeAbout} />
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
