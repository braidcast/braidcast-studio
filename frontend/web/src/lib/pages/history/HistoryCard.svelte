<script lang="ts">
  import { type SessionInfo } from "$lib/api/bridge";
  import { SESSION_END_LABEL, SESSION_END_STATE, STATE_COLOR_EXT } from "$lib/theme/stateColors";
  import PlatformMark from "$lib/ui/PlatformMark.svelte";
  import Thumb from "$lib/ui/Thumb.svelte";
  import { fmtDuration } from "$lib/utils/format";
  import { thumbDataUri } from "$lib/utils/thumbCache";

  interface Props {
    session: SessionInfo;
    onOpen: (id: string) => void;
  }
  let { session, onOpen }: Props = $props();

  const running = $derived(session.endedAt === null);
  const edge = $derived(running ? "live" : (SESSION_END_STATE[session.endReason] ?? "off"));
  const label = $derived(running ? "Live" : (SESSION_END_LABEL[session.endReason] ?? "Ended"));
  const duration = $derived(
    session.endedAt === null ? "" : fmtDuration(session.endedAt - session.startedAt),
  );
  const started = $derived(
    new Date(session.startedAt).toLocaleString(undefined, {
      dateStyle: "medium",
      timeStyle: "short",
    }),
  );

  // The stored path is relative and the browser cannot resolve it, so the file is
  // read on demand rather than shipped inline with every list row.
  let dataUri = $state("");
  $effect(() => {
    const file = session.thumbFile;
    let cancelled = false;
    void thumbDataUri(file).then((uri) => {
      if (!cancelled) {
        dataUri = uri;
      }
    });
    return () => {
      cancelled = true;
    };
  });
</script>

<button class="card" style="--edge: {STATE_COLOR_EXT[edge]}" onclick={() => onOpen(session.id)}>
  <div class="thumb-wrap"><Thumb src={dataUri} alt={session.title} /></div>
  <div class="body">
    <div class="title">{session.title || "Untitled stream"}</div>
    <div class="meta">
      <span class="state">{label}</span>
      {#if duration}<span>· {duration}</span>{/if}
      <span>· {started}</span>
    </div>
    {#if session.destinations.length > 0}
      <div class="marks">
        {#each session.destinations as d (d.profileId + d.platform)}
          <PlatformMark platform={d.platform} size={13} />
        {/each}
      </div>
    {/if}
  </div>
</button>

<style>
  .card {
    width: 100%;
    height: auto;
    display: flex;
    gap: 12px;
    padding: 12px 14px;
    border: 0;
    border-left: 2px solid var(--edge);
    border-bottom: var(--border-weight) solid var(--color-border-2);
    background: transparent;
    text-align: left;
  }
  .card:hover {
    background: color-mix(in srgb, var(--color-text) 4%, transparent);
  }
  .thumb-wrap {
    width: 84px;
    flex: 0 0 auto;
  }
  .body {
    min-width: 0;
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .title {
    font-size: 12px;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .meta {
    display: flex;
    gap: 4px;
    flex-wrap: wrap;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .state {
    color: var(--edge);
  }
  .marks {
    display: flex;
    gap: 6px;
    align-items: center;
  }
</style>
