<script lang="ts">
  import { obs, type AudioSource } from "../bridge";
  import { openFilters } from "../filterDialogOpener.svelte";
  import { openAdvAudio } from "../advAudioOpener.svelte";

  // Per-source faders + live dB meters. Levels arrive on the audio.levels push
  // (~30 Hz x N sources); we coalesce into a Map and flush once per animation
  // frame so layout is touched at most once a frame. The meter honors the
  // --meter-style token (segmented for Industrial, gradient for Graphite/Slate)
  // via :root[data-meter-style] (visual-style.html).
  // The mount adapter strips internal __* keys; this dock declares no props.
  let {}: Record<string, unknown> = $props();

  const DB_FLOOR = -60;
  const DB_CEIL = 0;

  function dbToPercent(db: number): number {
    if (!Number.isFinite(db) || db <= DB_FLOOR) {
      return 0;
    }
    if (db >= DB_CEIL) {
      return 100;
    }
    return ((db - DB_FLOOR) / (DB_CEIL - DB_FLOOR)) * 100;
  }

  let sources = $state<AudioSource[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  const latest = new Map<string, { magnitude: number; peak: number }>();
  let meters = $state<Record<string, { mag: number; peak: number }>>({});
  let rafHandle = 0;

  function scheduleFlush() {
    if (rafHandle) {
      return;
    }
    rafHandle = requestAnimationFrame(() => {
      rafHandle = 0;
      const next: Record<string, { mag: number; peak: number }> = {};
      for (const [uuid, lvl] of latest) {
        next[uuid] = { mag: dbToPercent(lvl.magnitude), peak: dbToPercent(lvl.peak) };
      }
      meters = next;
    });
  }

  async function load() {
    error = null;
    try {
      const res = await obs.call("audio.list");
      sources = res.sources;
      const live = new Set(sources.map((s) => s.uuid));
      for (const uuid of latest.keys()) {
        if (!live.has(uuid)) {
          latest.delete(uuid);
        }
      }
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void load();
    const offLevels = obs.on("audio.levels", (p) => {
      for (const l of p.levels) {
        const cur = latest.get(l.uuid);
        if (cur) {
          cur.magnitude = l.magnitude;
          cur.peak = l.peak;
        } else {
          latest.set(l.uuid, { magnitude: l.magnitude, peak: l.peak });
        }
      }
      scheduleFlush();
    });
    const offChanged = obs.on("audio.changed", () => void load());
    return () => {
      offLevels();
      offChanged();
      if (rafHandle) {
        cancelAnimationFrame(rafHandle);
        rafHandle = 0;
      }
    };
  });

  async function setDeflection(src: AudioSource, value: number) {
    src.deflection = value; // optimistic (mutating the $state element is reactive)
    try {
      const res = await obs.call("audio.setDeflection", { uuid: src.uuid, deflection: value });
      src.deflection = res.deflection;
      src.volumeDb = res.volumeDb;
    } catch (e) {
      error = (e as Error).message;
    }
  }

  function onFaderInput(src: AudioSource, e: Event) {
    void setDeflection(src, Number((e.currentTarget as HTMLInputElement).value));
  }

  async function toggleMuted(src: AudioSource) {
    const desired = !src.muted;
    src.muted = desired; // optimistic
    try {
      const res = await obs.call("audio.setMuted", { uuid: src.uuid, muted: desired });
      src.muted = res.muted;
    } catch (e) {
      error = (e as Error).message;
    }
  }

  function fmtDb(db: number): string {
    if (!Number.isFinite(db) || db <= DB_FLOOR) {
      return "-∞";
    }
    return (db > 0 ? "+" : "") + db.toFixed(1);
  }
</script>

<div class="dock-body">
  {#if error}
    <p class="dock-msg err">{error}</p>
  {/if}

  {#if !loaded}
    <p class="dock-msg">Loading…</p>
  {:else if sources.length === 0}
    <p class="dock-msg">No active audio sources</p>
  {:else}
    <ul class="list">
      {#each sources as src (src.uuid)}
        {@const m = meters[src.uuid]}
        <li class="row" class:muted={src.muted}>
          <div class="top">
            <span class="name" title={src.name}>{src.name}</span>
            <span class="db">{fmtDb(src.volumeDb)} dB</span>
          </div>
          <div class="meter" aria-hidden="true">
            <div class="unlit" style:width="{m ? 100 - m.mag : 100}%"></div>
            {#if m && m.peak > 0}
              <div class="peak" style:left="{m.peak}%"></div>
            {/if}
          </div>
          <div class="controls">
            <button
              class="mute"
              class:on={src.muted}
              title={src.muted ? "Unmute" : "Mute"}
              aria-pressed={src.muted}
              onclick={() => void toggleMuted(src)}>{src.muted ? "🔇" : "🔊"}</button
            >
            <button class="mute filters" title="Filters" onclick={() => openFilters(src.name, "audio")}>🎛</button>
            <button
              class="mute filters"
              title="Advanced Audio Properties"
              onclick={() => openAdvAudio(src.name, src.name)}>⚙</button
            >
            <input
              class="fader"
              type="range"
              min="0"
              max="1"
              step="0.01"
              value={src.deflection}
              aria-label="{src.name} volume"
              oninput={(e) => onFaderInput(src, e)}
            />
          </div>
        </li>
      {/each}
    </ul>
  {/if}
</div>

<style>
  .list {
    list-style: none;
    margin: 0;
    padding: 8px;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .row {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .top {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 8px;
  }
  .name {
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .row.muted .name {
    color: var(--color-muted);
    text-decoration: line-through;
  }
  .db {
    flex-shrink: 0;
    font-size: 10px;
    color: var(--color-muted);
    font-variant-numeric: tabular-nums;
  }
  /* The meter track is painted full-width with the zone gradient; an .unlit cover
     masks the un-reached portion from the right. --meter-style switches the track
     between a smooth gradient and a segmented (notched) fill. */
  .meter {
    position: relative;
    height: 8px;
    overflow: hidden;
    background-color: var(--color-base);
    background-image: linear-gradient(
      90deg,
      var(--meter-green) 0%,
      var(--meter-green) 58%,
      var(--meter-yellow) 78%,
      var(--meter-red) 100%
    );
  }
  :global(:root[data-meter-style="segmented"]) .meter {
    -webkit-mask-image: repeating-linear-gradient(90deg, #000 0 4px, transparent 4px 5px);
    mask-image: repeating-linear-gradient(90deg, #000 0 4px, transparent 4px 5px);
  }
  .unlit {
    position: absolute;
    top: 0;
    right: 0;
    bottom: 0;
    background: var(--color-base);
  }
  .peak {
    position: absolute;
    top: 0;
    bottom: 0;
    width: 2px;
    background: var(--color-text);
  }
  .controls {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .mute {
    flex-shrink: 0;
    height: auto;
    padding: 2px 6px;
    font-size: 12px;
    line-height: 1;
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-muted);
  }
  .mute.on {
    color: var(--color-live);
    border-color: var(--color-live);
  }
  .mute.filters:hover {
    color: var(--color-accent);
    border-color: var(--color-accent);
  }
  .fader {
    flex: 1;
    min-width: 0;
    accent-color: var(--color-accent);
  }
  /* Audio mixer messages use a roomier pad than the shared 8px 7px default. */
  .dock-msg {
    padding: 10px 9px;
  }
</style>
