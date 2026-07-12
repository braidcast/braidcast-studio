<script lang="ts">
  import GeneralTab from "../GeneralTab.svelte";
  import HotkeysTab from "../HotkeysTab.svelte";
  import AudioTab from "../AudioTab.svelte";
  import BrowserDocksTab from "../BrowserDocksTab.svelte";
  import AppearanceTab from "../AppearanceTab.svelte";
  import AdvancedTab from "../AdvancedTab.svelte";
  import DiagnosticsTab from "../DiagnosticsTab.svelte";
  import PageHeader from "../PageHeader.svelte";
  import { settingsNav, setSettingsTab, type SettingsTab } from "../settingsOpener.svelte";

  // Full Settings page: header + a 196px left sub-nav + a content pane that renders
  // the selected tab's component. AI/MCP is its own nav-rail page; output bindings
  // moved to the Stream page (enable/disable per canvas + destination). The page
  // model is live-apply — every tab persists through its own bridge calls, so
  // there's no OK/Apply/Cancel boundary.
  const tabs: { id: SettingsTab; label: string }[] = [
    { id: "general", label: "General" },
    { id: "audio", label: "Audio" },
    { id: "hotkeys", label: "Hotkeys" },
    { id: "browserDocks", label: "Browser Docks" },
    { id: "appearance", label: "Appearance" },
    { id: "advanced", label: "Advanced" },
    { id: "diagnostics", label: "Diagnostics" },
  ];
</script>

<div class="page">
  <PageHeader title="Settings" sub="application preferences" />

  <div class="body">
    <nav class="subnav" aria-label="Settings categories">
      {#each tabs as t (t.id)}
        <button
          class="nav-item"
          class:active={settingsNav.tab === t.id}
          aria-current={settingsNav.tab === t.id ? "page" : undefined}
          onclick={() => setSettingsTab(t.id)}>{t.label}</button
        >
      {/each}
    </nav>

    <div class="pane">
      {#if settingsNav.tab === "general"}
        <GeneralTab />
      {:else if settingsNav.tab === "audio"}
        <AudioTab />
      {:else if settingsNav.tab === "hotkeys"}
        <HotkeysTab />
      {:else if settingsNav.tab === "browserDocks"}
        <BrowserDocksTab />
      {:else if settingsNav.tab === "appearance"}
        <AppearanceTab />
      {:else if settingsNav.tab === "advanced"}
        <AdvancedTab />
      {:else if settingsNav.tab === "diagnostics"}
        <DiagnosticsTab />
      {/if}
    </div>
  </div>
</div>

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
    background: var(--color-base);
    color: var(--color-text);
  }
  .body {
    flex: 1;
    min-height: 0;
    display: flex;
  }
  .subnav {
    flex: 0 0 196px;
    border-right: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
    padding: 10px 0;
    overflow-y: auto;
  }
  .nav-item {
    display: block;
    width: 100%;
    text-align: left;
    padding: 9px 18px;
    font-family: var(--font-ui);
    font-size: 12px;
    font-weight: 400;
    background: transparent;
    border: 0;
    border-left: 2.5px solid transparent;
    color: var(--color-dim);
    cursor: pointer;
  }
  .nav-item:hover {
    color: var(--color-text);
  }
  .nav-item.active {
    font-weight: 600;
    background: color-mix(in srgb, var(--color-accent) 10%, transparent);
    border-left-color: var(--color-accent);
    color: var(--color-accent);
  }
  .pane {
    flex: 1;
    min-width: 0;
    overflow: auto;
    padding: 24px 28px;
  }
</style>
