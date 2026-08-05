<script lang="ts">
  import type { SessionDetail } from "$lib/api/bridge";
  import { sessionsStore } from "$lib/stores/sessionsStore.svelte";
  import Modal from "$lib/ui/Modal.svelte";
  import PlatformMark from "$lib/ui/PlatformMark.svelte";
  import { fmtBitrate, fmtDuration } from "$lib/utils/format";
  import { sparkArea, sparkPoints } from "$lib/utils/statsMeter";

  interface Props {
    id: string;
    onClose: () => void;
  }
  let { id, onClose }: Props = $props();

  const SW = 240;
  const SH = 36;

  let detail = $state<SessionDetail | null>(null);
  let error = $state<string | null>(null);

  $effect(() => {
    const wanted = id;
    let cancelled = false;
    detail = null;
    error = null;
    void sessionsStore
      .get(wanted)
      .then((d) => {
        if (!cancelled) {
          detail = d;
        }
      })
      .catch((e: unknown) => {
        if (!cancelled) {
          error = (e as Error).message;
        }
      });
    return () => {
      cancelled = true;
    };
  });

  // One sparkline per metric with its own min/max caption. sparkPoints draws an
  // unlabeled trend line -- no axes, no ticks, no hover readout -- so the numbers
  // have to be carried by the caption rather than read off the curve.
  const metrics = $derived.by(() => {
    const h = detail?.health ?? [];
    if (h.length === 0) {
      return [];
    }
    return [
      {
        k: "bitrate",
        series: h.map((p) => p.bitrateKbps),
        fmt: (v: number) => fmtBitrate(v),
      },
      {
        k: "dropped frames",
        series: h.map((p) => p.droppedFrames),
        fmt: (v: number) => String(Math.round(v)),
      },
      {
        k: "encode skipped",
        series: h.map((p) => p.encodeSkipped),
        fmt: (v: number) => String(Math.round(v)),
      },
      {
        k: "congestion",
        series: h.map((p) => p.congestionPct),
        fmt: (v: number) => v.toFixed(1) + "%",
      },
      {
        k: "cpu",
        series: h.map((p) => p.cpuPct),
        fmt: (v: number) => v.toFixed(1) + "%",
      },
    ];
  });

  const duration = $derived(
    detail && detail.endedAt !== null ? fmtDuration(detail.endedAt - detail.startedAt) : "",
  );
</script>

<Modal title={detail?.title || "Session"} {onClose} width={620}>
  {#if error}
    <p class="err">Could not load this session: {error}</p>
  {:else if !detail}
    <p class="muted">Loading…</p>
  {:else}
    <div class="head">
      <span class="muted">{new Date(detail.startedAt).toLocaleString()}</span>
      {#if duration}<span class="muted">· {duration}</span>{/if}
      <span class="muted">· {detail.endReason}</span>
    </div>

    <section>
      <h3>Destinations</h3>
      {#if detail.destinations.length === 0}
        <p class="muted">No destinations were recorded for this session.</p>
      {:else}
        {#each detail.destinations as d (d.profileId + d.platform)}
          <div class="dest">
            <div class="dest-head">
              <PlatformMark platform={d.platform} size={13} />
              <span class="dest-label">{d.accountLabel || d.platform}</span>
              <span class="muted">{d.finalState}</span>
            </div>
            {#if d.title}<div class="row"><span class="rk">title</span>{d.title}</div>{/if}
            {#if d.category}
              <div class="row"><span class="rk">category</span>{d.category}</div>
            {/if}
            {#if d.tags.length > 0}
              <div class="row"><span class="rk">tags</span>{d.tags.join(", ")}</div>
            {/if}
            {#if d.error}<div class="row err"><span class="rk">error</span>{d.error}</div>{/if}
          </div>
        {/each}
      {/if}
    </section>

    <section>
      <h3>Health</h3>
      {#if metrics.length === 0}
        <p class="muted">No health samples were recorded for this session.</p>
      {:else}
        {#each metrics as m (m.k)}
          {@const pts = sparkPoints(m.series, undefined, SW, SH)}
          {@const lo = Math.min(...m.series)}
          {@const hi = Math.max(...m.series)}
          <div class="metric">
            <span class="metric-k">{m.k}</span>
            <svg viewBox="0 0 {SW} {SH}" preserveAspectRatio="none" aria-hidden="true">
              {#if pts}
                <polygon class="spark-fill" points={sparkArea(pts, SW, SH)} />
                <polyline class="spark-line" points={pts} />
              {:else}
                <line class="spark-flat" x1="0" y1={SH - 1} x2={SW} y2={SH - 1} />
              {/if}
            </svg>
            <span class="metric-cap">min {m.fmt(lo)} · max {m.fmt(hi)}</span>
          </div>
        {/each}
      {/if}
    </section>
  {/if}
</Modal>

<style>
  .head {
    display: flex;
    gap: 4px;
    flex-wrap: wrap;
    margin-bottom: 12px;
  }
  section {
    margin-bottom: 16px;
  }
  h3 {
    margin: 0 0 8px;
    font-family: var(--font-mono);
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: var(--color-muted);
  }
  .muted {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .err {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-live);
  }
  .dest {
    padding: 8px 0;
    border-bottom: var(--border-weight) solid var(--color-border-2);
  }
  .dest-head {
    display: flex;
    align-items: center;
    gap: 6px;
    margin-bottom: 4px;
  }
  .dest-label {
    font-size: 12px;
    color: var(--color-text);
  }
  .row {
    display: flex;
    gap: 8px;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-text);
    line-height: 1.6;
  }
  .rk {
    min-width: 64px;
    color: var(--color-muted);
  }
  .metric {
    display: grid;
    grid-template-columns: 110px 1fr;
    align-items: center;
    gap: 8px;
    margin-bottom: 6px;
  }
  .metric-k,
  .metric-cap {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .metric-cap {
    grid-column: 2;
  }
  .metric svg {
    width: 100%;
    height: 36px;
  }
  .spark-fill {
    fill: var(--color-accent);
    opacity: 0.18;
  }
  .spark-line {
    fill: none;
    stroke: var(--color-accent);
    stroke-width: 1;
  }
  .spark-flat {
    stroke: var(--color-border);
    stroke-width: 1;
  }
</style>
