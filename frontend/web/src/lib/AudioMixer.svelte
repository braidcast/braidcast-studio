<script lang="ts">
  import { obs, type AudioSource } from "./bridge";

  // dB range mapped to the meter/peak fill (0..100%). OBS volmeters report
  // roughly -60..0 dB; below the floor reads as silence.
  const DB_FLOOR = -60;
  const DB_CEIL = 0;

  // Map a dB value to a 0..100 fill percentage (linear over the clamped range;
  // good enough for v1 per the brief).
  function dbToPercent(db: number): number {
    if (!Number.isFinite(db) || db <= DB_FLOOR) return 0;
    if (db >= DB_CEIL) return 100;
    return ((db - DB_FLOOR) / (DB_CEIL - DB_FLOOR)) * 100;
  }

  let sources = $state<AudioSource[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  // Levels are pushed at ~30 Hz x N sources. We accumulate the latest value per
  // uuid into a mutable Map (no per-event allocation) and flush to reactive
  // state once per animation frame so layout is touched at most once per frame.
  const latest = new Map<string, { magnitude: number; peak: number }>();
  let meters = $state<Record<string, { mag: number; peak: number }>>({});
  let rafHandle = 0;

  function scheduleFlush() {
    if (rafHandle) return;
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
      // Drop level entries for sources that went away.
      const live = new Set(sources.map((s) => s.uuid));
      for (const uuid of latest.keys()) {
        if (!live.has(uuid)) latest.delete(uuid);
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

  // Optimistic update on input; reconcile from the authoritative response.
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
    const value = Number((e.currentTarget as HTMLInputElement).value);
    void setDeflection(src, value);
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
    if (!Number.isFinite(db) || db <= DB_FLOOR) return "-∞";
    return (db > 0 ? "+" : "") + db.toFixed(1);
  }
</script>

<section class="panel">
  <header>
    <h2>Audio Mixer</h2>
  </header>

  {#if error}
    <p class="error">{error}</p>
  {/if}

  {#if !loaded}
    <p class="dim">Loading…</p>
  {:else if sources.length === 0}
    <p class="dim">No active audio sources</p>
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
            <div class="fill" style:width="{m ? m.mag : 0}%"></div>
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
</section>

<style>
  .panel {
    border: 1px solid var(--border);
    border-radius: 10px;
    background: var(--bg-raised);
    padding: 14px 16px;
    display: flex;
    flex-direction: column;
    gap: 10px;
    min-height: 0;
    overflow: auto;
  }
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  h2 {
    margin: 0;
    font-size: 13px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-dim);
  }
  .list {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .row {
    display: flex;
    flex-direction: column;
    gap: 5px;
    padding: 8px 10px;
    border: 1px solid var(--border);
    border-radius: 8px;
    background: var(--bg-sunken);
  }
  .top {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 8px;
    min-width: 0;
  }
  .name {
    color: var(--text);
    font-size: 13px;
    font-weight: 500;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .row.muted .name {
    color: var(--text-dim);
  }
  .db {
    color: var(--text-dim);
    font-size: 11px;
    font-variant-numeric: tabular-nums;
    flex-shrink: 0;
  }
  .meter {
    position: relative;
    height: 8px;
    border-radius: 4px;
    background: var(--bg-raised);
    border: 1px solid var(--border);
    overflow: hidden;
  }
  .fill {
    position: absolute;
    inset: 0 auto 0 0;
    background: var(--accent);
    border-radius: 4px 0 0 4px;
    transition: width 80ms linear;
  }
  .row.muted .fill {
    background: var(--text-dim);
  }
  .peak {
    position: absolute;
    top: 0;
    bottom: 0;
    width: 2px;
    background: var(--text-soft);
    transform: translateX(-1px);
  }
  .controls {
    display: flex;
    align-items: center;
    gap: 10px;
  }
  .mute {
    background: none;
    border: 1px solid var(--border);
    border-radius: 6px;
    color: var(--text-soft);
    cursor: pointer;
    font-size: 13px;
    line-height: 1;
    padding: 4px 8px;
    flex-shrink: 0;
  }
  .mute:hover {
    color: var(--text);
    background: var(--bg-raised);
  }
  .mute.on {
    color: var(--off);
    border-color: var(--off);
  }
  .fader {
    flex: 1;
    accent-color: var(--accent);
    cursor: pointer;
    min-width: 0;
  }
  .dim {
    color: var(--text-dim);
    margin: 0;
    font-size: 12px;
  }
  .error {
    color: var(--off);
    margin: 0;
    font-size: 12px;
  }
</style>
