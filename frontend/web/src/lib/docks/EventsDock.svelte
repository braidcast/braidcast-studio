<script lang="ts">
  import { obs, type NormalizedEvent, type ChatPlatform, type EventType } from "../bridge";
  import {
    PLATFORM_COLORS,
    PLATFORM_LABELS,
    EVENT_TYPE_COLORS,
    EVENT_TYPE_LABELS,
  } from "../theme/platformColors";
  import { FeedVirtualizer, type FeedRow } from "../feedVirtualizer.svelte";
  import EmptyState from "../EmptyState.svelte";
  import Icon from "../dock/Icon.svelte";
  import { oauthStore } from "../oauthStore.svelte";

  // Host supplies tab chrome + strips __* keys; this body declares no props.
  let {}: Record<string, unknown> = $props();

  // Platform dot/tag color + label (matches the Multichat dock).
  const PLATFORM_COLOR = PLATFORM_COLORS;
  const PLATFORM_LABEL = PLATFORM_LABELS;

  // Human labels per event type -- the summary carries the phrasing; this is the
  // fallback the summary/aria fall back to for an unknown type.
  const TYPE_LABEL = EVENT_TYPE_LABELS;

  // Accent color per type. follow=blue; sub/resub=purple; subgift/member=gold;
  // cheer=teal (bits); raid=orange; superchat/supersticker=green (money).
  const TYPE_COLOR = EVENT_TYPE_COLORS;

  // Format a money amount given in MINOR currency units (cents). Prefers the
  // locale currency formatter; an unknown/invalid currency code throws, so wrap it
  // and fall back to a bare `${value} ${code}`. Callers only invoke this when
  // amount != null.
  function money(amount: number, currency: string | undefined): string {
    const value = amount / 100;
    if (currency) {
      try {
        return new Intl.NumberFormat(undefined, { style: "currency", currency }).format(value);
      } catch {
        // invalid ISO 4217 code -- fall through to the plain form below.
      }
    }
    return `${value.toFixed(2)} ${currency ?? ""}`.trim();
  }

  // One-line action summary per type (excludes `message`, which is bound
  // separately so it renders as escaped text). Registry map, not a switch, so a
  // new type is a single entry. Unknown types fall back to the type label.
  const SUMMARY: Record<EventType, (e: NormalizedEvent) => string> = {
    follow: () => "followed",
    sub: (e) => "subscribed" + (e.tier ? ` · ${e.tier}` : ""),
    resub: (e) => "resubscribed" + (e.months ? ` · ${e.months} months` : ""),
    subgift: (e) => {
      const n = e.count ?? 1;
      return `gifted ${n} sub${n === 1 ? "" : "s"}` + (e.tier ? ` · ${e.tier}` : "");
    },
    cheer: (e) => `cheered ${e.amount ?? 0} bits`,
    raid: (e) => {
      const n = e.amount ?? 0;
      return `raided with ${n} viewer${n === 1 ? "" : "s"}`;
    },
    superchat: (e) => "Super Chat" + (e.amount != null ? ` ${money(e.amount, e.currency)}` : ""),
    supersticker: (e) => "Super Sticker" + (e.amount != null ? ` ${money(e.amount, e.currency)}` : ""),
    member: (e) =>
      e.months ? `member · ${e.months} months` : e.tier ? `became a member · ${e.tier}` : "became a member",
  };

  function summary(e: NormalizedEvent): string {
    const fn = SUMMARY[e.type];
    return fn ? fn(e) : (TYPE_LABEL[e.type] ?? e.type);
  }

  // --- feed (ring-capped + virtualized) -------------------------------------
  // Platform filter: "all" or one platform. Filtering feeds the virtualizer a
  // derived subset -- heights stay keyed by the stable clientKey, so a filtered-out
  // row keeps its measured height and re-appears at the right size when re-shown.
  let filter = $state<"all" | ChatPlatform>("all");

  // Platform chips are gated on logged-in OAuth accounts, not the fixed platform list:
  // 0 connected -> empty state, no chips; 1 -> just that platform (no "All", which is
  // meaningless with one); >=2 -> "All" + each connected platform. Live via oauthStore.
  $effect(() => oauthStore.subscribe());
  let connectedPlatforms = $derived(oauthStore.connectedPlatforms);
  let showAllChip = $derived(connectedPlatforms.length >= 2);

  const feed = new FeedVirtualizer<NormalizedEvent>({ max: 500, estimate: 38, getDisplay: () => filtered });
  // Explicitly typed to break the feed <-> filtered inference cycle (getDisplay
  // closes over filtered, which reads feed.rows).
  let filtered: FeedRow<NormalizedEvent>[] = $derived(
    filter === "all" ? feed.rows : feed.rows.filter((r) => r.item.platform === filter),
  );
  const measureRow = feed.measureRow;
  const feedScroll = feed.scroll;

  // Keep the active filter valid as accounts connect/disconnect: with exactly one
  // platform, pin the filter to it (its chip is the only one, so it must read active);
  // otherwise drop a filter whose platform is no longer connected back to "all".
  $effect(() => {
    if (connectedPlatforms.length === 1) {
      if (filter !== connectedPlatforms[0]) {
        filter = connectedPlatforms[0];
        feed.restick();
      }
    } else if (filter !== "all" && !connectedPlatforms.includes(filter as ChatPlatform)) {
      filter = "all";
      feed.restick();
    }
  });

  // Switching the filter changes the visible set; re-pin to the newest of the new
  // subset so the user always lands on the latest matching event.
  function setFilter(f: "all" | ChatPlatform): void {
    filter = f;
    feed.restick();
  }

  function clear(): void {
    // The host clears its store then emits an empty events.backfill; setFeed([])
    // below is a local echo so the feed empties immediately even if the push lags.
    obs.call("events.clear").catch(() => {});
    feed.setFeed([], true);
  }

  $effect(() => {
    obs
      .call("events.list")
      // events.list / events.backfill arrive newest-first; setFeed reverses into
      // oldest->newest (top->bottom) to match the enqueue-at-bottom order.
      .then((list) => feed.setFeed(list, true))
      .catch(() => {});
    const offNew = obs.on("events.new", (e) => feed.enqueue(e));
    const offBackfill = obs.on("events.backfill", (batch) => feed.setFeed(batch, true));
    return () => {
      offNew();
      offBackfill();
      feed.dispose();
    };
  });
</script>

<!-- Feather/lucide-style line icons (24x24, currentColor) matching the nav rail. -->
{#snippet typeIcon(type: EventType)}
  <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">
    {#if type === "follow"}
      <path d="M20.8 5.6a5 5 0 0 0-7.1 0L12 7.3l-1.7-1.7a5 5 0 0 0-7.1 7.1l1.7 1.7L12 21.4l7.4-7.3 1.4-1.4a5 5 0 0 0 0-7.1z" />
    {:else if type === "sub"}
      <path d="M12 2.5l2.9 6 6.6.6-5 4.3 1.5 6.4L12 16.9 6 20.3l1.5-6.4-5-4.3 6.6-.6z" />
    {:else if type === "resub"}
      <path d="M17 2l4 4-4 4" />
      <path d="M3 11V9a4 4 0 0 1 4-4h14" />
      <path d="M7 22l-4-4 4-4" />
      <path d="M21 13v2a4 4 0 0 1-4 4H3" />
    {:else if type === "subgift"}
      <rect x="3" y="8" width="18" height="4" />
      <path d="M12 8v13" />
      <path d="M19 12v9H5v-9" />
      <path d="M7.5 8a2.5 2.5 0 0 1 0-5C11 3 12 8 12 8" />
      <path d="M16.5 8a2.5 2.5 0 0 0 0-5C13 3 12 8 12 8" />
    {:else if type === "cheer"}
      <polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2" />
    {:else if type === "raid"}
      <path d="M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2" />
      <circle cx="9" cy="7" r="4" />
      <path d="M23 21v-2a4 4 0 0 0-3-3.87" />
      <path d="M16 3.13a4 4 0 0 1 0 7.75" />
    {:else if type === "superchat"}
      <line x1="12" y1="1.5" x2="12" y2="22.5" />
      <path d="M17 5H9.5a3.5 3.5 0 0 0 0 7h5a3.5 3.5 0 0 1 0 7H6" />
    {:else if type === "supersticker"}
      <circle cx="12" cy="12" r="10" />
      <path d="M8 14s1.5 2 4 2 4-2 4-2" />
      <line x1="9" y1="9" x2="9.01" y2="9" />
      <line x1="15" y1="9" x2="15.01" y2="9" />
    {:else if type === "member"}
      <path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z" />
    {:else}
      <circle cx="12" cy="12" r="4" fill="currentColor" stroke="none" />
    {/if}
  </svg>
{/snippet}

<div class="events">
  {#if connectedPlatforms.length > 0}
    <div class="bar">
      {#if showAllChip}
        <button class="chip" class:on={filter === "all"} onclick={() => setFilter("all")}>All</button>
      {/if}
      {#each connectedPlatforms as p (p)}
        <button
          class="chip"
          class:on={filter === p}
          style:--chip={PLATFORM_COLOR[p]}
          onclick={() => setFilter(p)}
        >
          <span class="cdot" style:background={PLATFORM_COLOR[p]}></span>
          {PLATFORM_LABEL[p]}
        </button>
      {/each}
    </div>
  {/if}

  <div class="scroll" use:feedScroll>
    {#if connectedPlatforms.length === 0}
      <EmptyState compact title="Connect an account to see events." />
    {:else if feed.rows.length === 0}
      <EmptyState compact title="Follows, subs, gifts and cheers from your connected accounts appear here." />
    {:else if filtered.length === 0}
      <EmptyState compact title={"No " + (filter === "all" ? "" : PLATFORM_LABEL[filter] + " ") + "events yet."} />
    {:else}
      <div class="sizer" style:height={feed.layout.total + "px"}>
        {#each feed.visible as row (row.clientKey)}
          {@const e = row.item}
          {@const platformColor = PLATFORM_COLOR[e.platform] ?? "var(--color-muted)"}
          {@const actorColor = e.actorColor || platformColor}
          {@const accent = TYPE_COLOR[e.type] ?? "var(--color-muted)"}
          <div class="row" style:top={row.top + "px"} use:measureRow={row.clientKey}>
            <div class="line">
              <span class="pdot" style:background={platformColor} title={PLATFORM_LABEL[e.platform] ?? e.platform}
              ></span>
              <span class="icon" style:color={accent} title={TYPE_LABEL[e.type] ?? e.type}
                >{@render typeIcon(e.type)}</span
              >
              <span class="actor" style:color={actorColor}>{e.actorName}</span>
              <span class="sum">{summary(e)}</span>
            </div>
            {#if e.message}<span class="msg">{e.message}</span>{/if}
          </div>
        {/each}
      </div>
    {/if}
  </div>

  {#if !feed.autoStick && filtered.length > 0}
    <button class="jump" onclick={feed.jumpToLatest}><Icon name="jump-down" size={11} /> Jump to latest</button>
  {/if}

  <div class="footer">
    <button class="clearbtn" disabled={feed.rows.length === 0} onclick={clear}>Clear</button>
  </div>
</div>

<style>
  .events {
    height: 100%;
    display: flex;
    flex-direction: column;
    background: var(--color-surface);
    font-family: var(--font-ui);
    min-height: 0;
    position: relative;
  }
  .bar {
    flex: 0 0 auto;
    display: flex;
    flex-wrap: wrap;
    gap: 4px;
    padding: 6px 8px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
  }
  .chip {
    display: flex;
    align-items: center;
    gap: 4px;
    padding: 3px 8px;
    font-size: 10px;
    font-family: var(--font-ui);
    color: var(--color-dim);
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    cursor: pointer;
  }
  .chip.on {
    border-color: var(--chip, var(--color-accent));
    color: var(--chip, var(--color-accent));
    background: color-mix(in srgb, var(--chip, var(--color-accent)) 14%, transparent);
  }
  .cdot {
    width: 6px;
    height: 6px;
    flex: 0 0 auto;
  }
  .scroll {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    overflow-x: hidden;
  }
  /* Absolute-positioned rows over a sized spacer = virtualized list (only the
     visible window is in the DOM; the sizer reserves the full scroll height). */
  .sizer {
    position: relative;
    width: 100%;
  }
  .row {
    position: absolute;
    left: 0;
    right: 0;
    padding: 4px 10px;
    font-size: 12px;
    line-height: 1.5;
    color: var(--color-text);
  }
  .line {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 0 6px;
  }
  .pdot {
    align-self: center;
    width: 7px;
    height: 7px;
    flex: 0 0 auto;
  }
  .icon {
    align-self: center;
    flex: 0 0 auto;
    display: inline-flex;
  }
  .icon :global(svg) {
    width: 13px;
    height: 13px;
    display: block;
  }
  .actor {
    font-weight: 600;
    overflow-wrap: anywhere;
  }
  .sum {
    color: var(--color-dim);
    overflow-wrap: anywhere;
  }
  .msg {
    display: block;
    margin-top: 1px;
    padding-left: 26px;
    color: var(--color-text);
    overflow-wrap: anywhere;
  }

  .jump {
    position: absolute;
    left: 50%;
    transform: translateX(-50%);
    bottom: 46px;
    z-index: 2;
    display: flex;
    align-items: center;
    gap: 5px;
    padding: 4px 12px;
    font-size: 10px;
    font-family: var(--font-ui);
    color: var(--color-accent-ink);
    background: var(--color-accent);
    border: 0;
    cursor: pointer;
  }

  .footer {
    flex: 0 0 auto;
    display: flex;
    justify-content: flex-end;
    padding: 6px 8px;
    border-top: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
  }
  .clearbtn {
    padding: 3px 12px;
    font-size: 10px;
    font-family: var(--font-ui);
    color: var(--color-dim);
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    cursor: pointer;
  }
  .clearbtn:disabled {
    opacity: 0.5;
    cursor: default;
  }
</style>
