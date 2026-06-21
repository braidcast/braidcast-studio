<script lang="ts">
  import { obs } from "./lib/bridge";
  import TopBar from "./lib/TopBar.svelte";
  import ScenesPanel from "./lib/ScenesPanel.svelte";
  import SourcesPanel from "./lib/SourcesPanel.svelte";
  import MultistreamPanel from "./lib/MultistreamPanel.svelte";
  import PreviewArea from "./lib/PreviewArea.svelte";
  import EventLog from "./lib/EventLog.svelte";

  let version = $state<string>("…");

  $effect(() => {
    obs
      .call("getVersion")
      .then((v) => (version = v || "unknown"))
      .catch((e) => (version = "error: " + (e as Error).message));
  });
</script>

<div class="shell">
  <TopBar />

  <main>
    <aside class="left">
      <ScenesPanel />
      <SourcesPanel />
      <MultistreamPanel />
    </aside>

    <div class="center">
      <PreviewArea />
      <EventLog />
    </div>
  </main>

  <footer class="statusbar">
    <span>libobs {version}</span>
  </footer>
</div>

<style>
  .shell {
    display: flex;
    flex-direction: column;
    height: 100%;
  }
  main {
    flex: 1;
    display: grid;
    grid-template-columns: 280px 1fr;
    gap: 14px;
    padding: 14px;
    min-height: 0;
  }
  .left {
    display: flex;
    flex-direction: column;
    gap: 14px;
    min-height: 0;
    overflow: auto;
  }
  .center {
    display: flex;
    flex-direction: column;
    gap: 14px;
    min-height: 0;
  }
  .statusbar {
    border-top: 1px solid var(--border);
    background: var(--bg-raised);
    padding: 6px 18px;
    font-size: 12px;
    color: var(--text-dim);
  }
</style>
