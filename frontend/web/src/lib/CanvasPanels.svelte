<script lang="ts">
  import { obs, type CanvasInfo, type MultistreamStatus } from "./bridge";
  import CanvasPanel from "./CanvasPanel.svelte";
  import { canvasFocus, setFocusedCanvas } from "./canvasFocus.svelte";

  let canvases = $state<CanvasInfo[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);
  // bindingUuid is per-output; we key status by canvasUuid (first row wins) so
  // each panel shows one representative dot for its bound output(s).
  let statusByCanvas = $state<Record<string, MultistreamStatus>>({});

  const enabled = $derived(canvases.filter((c) => c.enabled));

  async function load() {
    try {
      canvases = await obs.call("canvas.list");
      error = null;
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  // Keep the focused canvas valid: default to the Default canvas (or first
  // enabled) and re-pick when the focused panel disappears.
  $effect(() => {
    const list = enabled;
    if (list.length === 0) {
      canvasFocus.uuid = null;
      return;
    }
    const stillThere = list.some((c) => c.uuid === canvasFocus.uuid);
    if (!stillThere) {
      const def = list.find((c) => c.isDefault) ?? list[0];
      setFocusedCanvas(def.uuid);
    }
  });

  function applyStatuses(rows: MultistreamStatus[]) {
    const next: Record<string, MultistreamStatus> = {};
    for (const r of rows) {
      // Prefer the most "active" row per canvas for the dot.
      const prev = next[r.canvasUuid];
      if (!prev || rank(r.state) > rank(prev.state)) next[r.canvasUuid] = r;
    }
    statusByCanvas = next;
  }

  function rank(s: MultistreamStatus["state"]): number {
    return s === "live" ? 3 : s === "connecting" ? 2 : s === "error" ? 1 : 0;
  }

  $effect(() => {
    void load();
    const offCanvas = obs.on("canvas.changed", () => void load());
    // Enabling/disabling an output flips a canvas's `enabled` gate.
    const offBinding = obs.on("outputBinding.changed", () => void load());
    const offStatus = obs.on("multistream.changed", (p) => applyStatuses(p.outputs ?? []));
    // Seed status once.
    obs
      .call("multistream.status")
      .then((r) => applyStatuses(r.outputs ?? []))
      .catch(() => {});
    return () => {
      offCanvas();
      offBinding();
      offStatus();
    };
  });
</script>

<section class="canvas-panels">
  {#if error}
    <p class="error">{error}</p>
  {:else if !loaded}
    <p class="dim">Loading canvases…</p>
  {:else if enabled.length === 0}
    <div class="empty">
      <p class="empty-title">No active canvases</p>
      <p class="dim">Enable an output in Settings → Outputs.</p>
    </div>
  {:else}
    <div class="grid">
      {#each enabled as c (c.uuid)}
        <CanvasPanel canvas={c} status={statusByCanvas[c.uuid] ?? null} />
      {/each}
    </div>
  {/if}
</section>

<style>
  .canvas-panels {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
  }
  .grid {
    flex: 1;
    min-height: 0;
    display: grid;
    /* Uniform panels; each fills its track. Wraps to more columns as width
       allows -- portrait panels stay narrow naturally via their own layout. */
    grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
    grid-auto-rows: 1fr;
    gap: 14px;
    overflow: auto;
  }
  .empty {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 4px;
    text-align: center;
    border: 1px dashed var(--border);
    border-radius: 10px;
  }
  .empty-title {
    color: var(--text-soft);
    font-size: 14px;
    margin: 0;
  }
  .dim {
    color: var(--text-dim);
    margin: 0;
  }
  .error {
    color: var(--off, #d65a5a);
    margin: 0;
    font-size: 12px;
  }
</style>
