<script lang="ts">
  import { obs } from "./bridge";
  import SettingsModal from "./SettingsModal.svelte";

  let live = $state(false);
  let busy = $state(false);
  let settingsOpen = $state(false);

  async function refresh() {
    try {
      const st = await obs.call("getStreamingState");
      live = !!st?.active;
    } catch {
      // leave previous state; status bar surfaces hard failures elsewhere
    }
  }

  async function toggle() {
    busy = true;
    try {
      const r = await obs.call(live ? "streaming.stop" : "streaming.start");
      live = !!r?.active;
    } catch (e) {
      console.log("OBSLOG: streaming toggle ERR: " + (e as Error).message);
    } finally {
      busy = false;
    }
  }

  $effect(() => {
    refresh();
    return obs.on("streaming.changed", (p) => (live = !!p?.active));
  });
</script>

<header class="topbar">
  <div class="brand">
    <span class="dot" class:live></span>
    OBS MultiStreamer
  </div>
  <div class="actions">
    <button class="gear" title="Settings" aria-label="Settings" onclick={() => (settingsOpen = true)}>⚙</button>
    <button class:live onclick={toggle} disabled={busy}>
      {live ? "Stop Streaming" : "Start Streaming"}
    </button>
  </div>
</header>

{#if settingsOpen}
  <SettingsModal onClose={() => (settingsOpen = false)} />
{/if}

<style>
  .topbar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 12px 18px;
    border-bottom: 1px solid var(--border);
    background: var(--bg-raised);
  }
  .brand {
    display: flex;
    align-items: center;
    gap: 10px;
    font-weight: 600;
    font-size: 15px;
  }
  .dot {
    width: 9px;
    height: 9px;
    border-radius: 50%;
    background: var(--off);
  }
  .dot.live {
    background: var(--on);
    box-shadow: 0 0 8px var(--on);
  }
  .actions {
    display: flex;
    align-items: center;
    gap: 10px;
  }
  .gear {
    background: var(--bg-sunken);
    border: 1px solid var(--border);
    color: var(--text-soft);
    border-radius: 6px;
    padding: 6px 10px;
    font-size: 15px;
    line-height: 1;
    cursor: pointer;
  }
  .gear:hover {
    color: var(--text);
  }
  button.live {
    background: var(--off);
  }
  button.live:hover {
    background: #ef4444;
  }
</style>
