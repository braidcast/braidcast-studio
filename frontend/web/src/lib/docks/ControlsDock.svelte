<script lang="ts">
  import { obs } from "../bridge";
  import { openSettings } from "../settingsOpener.svelte";

  // Controls dock per dock-model.html: Go Live (real streaming start/stop, kept
  // from the classic Controls) + Record/Studio (stubs, wired later) + Settings ⚙
  // (opens the existing Settings modal). Live state stays in sync via the existing
  // streaming.changed push. Declared with the registry's pass-through props shape
  // (the mount adapter strips internal __* keys, so this dock reads none of them).
  let {}: Record<string, unknown> = $props();

  let active = $state(false);
  let busy = $state(false);

  $effect(() => {
    obs
      .call("getStreamingState")
      .then((s) => (active = s.active))
      .catch(() => {});
    const off = obs.on("streaming.changed", (p) => (active = p.active));
    return off;
  });

  async function toggleLive() {
    busy = true;
    try {
      const r = await obs.call(active ? "streaming.stop" : "streaming.start");
      active = r.active;
    } catch {
      // surfaced elsewhere; keep prior state
    } finally {
      busy = false;
    }
  }
</script>

<div class="controls">
  <button class="accent live" class:on={active} disabled={busy} onclick={toggleLive}>
    {active ? "■ STOP" : "● GO LIVE"}
  </button>
  <button disabled>RECORD</button>
  <button disabled>STUDIO</button>
  <button onclick={() => openSettings("canvases")}>SETTINGS ⚙</button>
</div>

<style>
  .controls {
    height: 100%;
    display: flex;
    flex-direction: column;
    gap: 6px;
    padding: 8px;
    background: var(--color-surface);
  }
  .controls button {
    width: 100%;
    font-family: var(--font-ui);
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  .live.on {
    background: var(--color-live);
    border-color: var(--color-live);
    color: #fff;
  }
</style>
