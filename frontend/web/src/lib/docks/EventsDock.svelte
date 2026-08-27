<script lang="ts">
  import { obs, type NormalizedEvent, type EventType } from "$lib/api/bridge";
  import { EV } from "$lib/utils/eventNames";
  import { callOrToast } from "$lib/utils/callToast";
  import { PLATFORM_COLORS, EVENT_TYPE_COLORS, EVENT_TYPE_LABELS } from "$lib/theme/platformColors";
  import { FeedVirtualizer, type FeedRow } from "$lib/utils/feedVirtualizer.svelte";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import Avatar from "$lib/ui/Avatar.svelte";
  import PlatformMark from "$lib/ui/PlatformMark.svelte";
  import DestinationChips, { type DestinationChipStatus } from "$lib/ui/DestinationChips.svelte";
  import { EVENTS_STATE_NOTE, eventsTransportFor } from "$lib/ui/destinationHealth";
  import {
    ALL_DESTINATIONS,
    attribute,
    destinationsByAccount,
    matchesSelection,
    reconcileSelection,
    selectionLabel,
    unarmedHint as unarmedHintFor,
    unarmedPlatforms as unarmedPlatformsOf,
    type DestinationSelection,
    type Fidelity,
  } from "$lib/ui/destinationSelection";
  import { oauthStore } from "$lib/stores/oauthStore.svelte";
  import { destinationIdentityStore, type DestinationIdentity } from "$lib/stores/destinationIdentityStore.svelte";
  import { transportHealthStore } from "$lib/stores/transportHealthStore.svelte";

  // Host supplies tab chrome + strips __* keys; this body declares no props.
  let {}: Record<string, unknown> = $props();

  // Platform dot/tag color (matches the Multichat dock).
  const PLATFORM_COLOR = PLATFORM_COLORS;

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
  // A YouTube subscribe normalizes to `follow` (youtube_events.cpp:188) because it is
  // the free channel follow, not the paid membership -- but the platforms NAME that act
  // differently, and a YouTube row reading "followed" describes something the viewer
  // never did. Overrides only: "followed" is the default, so a new platform needs an
  // entry here only where it disagrees.
  const FOLLOW_VERB: Partial<Record<NormalizedEvent["platform"], string>> = {
    youtube: "subscribed",
  };

  const SUMMARY: Record<EventType, (e: NormalizedEvent) => string> = {
    follow: (e) => FOLLOW_VERB[e.platform] ?? "followed",
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

  // --- destination attribution ----------------------------------------------
  // Which of the four live streams produced this event? "YouTube" stopped being an
  // answer once two channels each ran two orientations, so every row names the
  // channel and the canvas -- and, where the event genuinely cannot name a canvas,
  // says so instead of picking one. The tiering is shared with Chat
  // (ui/destinationSelection.ts, which documents which sources stamp what); the
  // wording below is this dock's, because an event and a message read differently.

  const FIDELITY_HINT: Record<Fidelity, string> = {
    exact: "Exact — the event named the broadcast it arrived on.",
    single:
      "Inferred — a channel-wide event, but this channel has exactly one armed destination, " +
      "so there is no other stream it could belong to.",
    wide:
      "Channel-wide — this event fired against the channel, not one broadcast. " +
      "Naming a canvas here would be a guess.",
    pending: "This destination's canvas has not loaded yet, or was deleted.",
    none: "No stream profile is configured for the account this event came from.",
  };

  $effect(() => {
    destinationIdentityStore.start();
  });

  // Only account-backed profiles can ever produce an event: a key/RTMP/WHIP profile
  // has no transport, so a chip for it could never match a row.
  let destinations = $derived(destinationIdentityStore.all.filter((d) => d.accountId !== ""));
  let destByUuid = $derived(new Map(destinations.map((d) => [d.profileUuid, d])));

  // The store is profile-keyed; the channel-wide fan-out needs the reverse index, and
  // it is an index over the store's own rows rather than a second identity join.
  let destByAccount = $derived(destinationsByAccount(destinations));

  // --- feed (ring-capped + virtualized) -------------------------------------
  // Three filter levels, all reachable: everything, one platform (both YouTube
  // channels at once, which is the view platform separation always gave), or one
  // specific stream. Filtering feeds the virtualizer a derived subset -- heights stay
  // keyed by the stable clientKey, so a filtered-out row keeps its measured height and
  // re-appears at the right size when re-shown.
  let filter = $state<DestinationSelection>(ALL_DESTINATIONS);

  // Chips are gated on what can actually originate an event, not on the fixed platform
  // list. A platform with a connected account but no stream profile still runs its
  // transport, so it gets a disabled chip that says why it cannot be filtered rather
  // than no chip at all.
  $effect(() => oauthStore.subscribe());
  let connectedPlatforms = $derived(oauthStore.connectedPlatforms);
  let unarmedPlatforms = $derived(unarmedPlatformsOf(connectedPlatforms, destinations));
  // More than one place an event can come from: the point at which every row has to
  // say which destination it belongs to.
  let multiOrigin = $derived(destinations.length + unarmedPlatforms.length >= 2);

  let activeDest = $derived(filter.kind === "destination" ? (destByUuid.get(filter.profileUuid) ?? null) : null);
  let filterLabel = $derived(selectionLabel(filter, destByUuid, { separator: " · ", all: "" }));
  // How many other streams of this channel also show its channel-wide events.
  let sharedWith = $derived(activeDest ? (destByAccount.get(activeDest.accountId)?.length ?? 1) - 1 : 0);
  let sharedNotice = $derived.by(() => {
    const d = activeDest;
    if (!d || sharedWith < 1) {
      return "";
    }
    return `Channel-wide ${d.displayName} events appear here and under its ${sharedWith} other stream${
      sharedWith === 1 ? "" : "s"
    }.`;
  });

  function unarmedHint(platform: string): string {
    return unarmedHintFor(platform, "its events cannot be filtered.");
  }

  // --- transport health -------------------------------------------------------
  // Resolution and wording live in ui/destinationHealth.ts, shared with the Chat dock
  // so the two surfaces cannot describe one account's transports differently.
  $effect(() => transportHealthStore.subscribe());

  const NO_TRANSPORT_NOTE = "no event transport — nothing has been opened for this account";

  // Absence of a row is UNKNOWN, not healthy: no state, so the chip's edge claims none.
  // The chip stays selectable regardless -- here it is only a filter, and a filter over
  // an empty set is still a legible answer.
  function statusOf(d: DestinationIdentity): DestinationChipStatus {
    const row = eventsTransportFor(d);
    if (!row) {
      return { note: NO_TRANSPORT_NOTE };
    }
    const parts = [EVENTS_STATE_NOTE[row.state]];
    if (row.lastError) {
      parts.push(row.lastError);
    }
    return { state: row.state, note: parts.join(" — ") };
  }

  const feed = new FeedVirtualizer<NormalizedEvent>({ max: 500, estimate: 38, getDisplay: () => filtered });
  // Explicitly typed to break the feed <-> filtered inference cycle (getDisplay
  // closes over filtered, which reads feed.rows).
  let filtered: FeedRow<NormalizedEvent>[] = $derived(
    filter.kind === "all" ? feed.rows : feed.rows.filter((r) => matchesSelection(r.item, filter, destByUuid)),
  );
  const measureRow = feed.measureRow;
  const feedScroll = feed.scroll;

  // Keep the active filter valid as destinations come and go.
  $effect(() => {
    const next = reconcileSelection(filter, destinations, destByUuid, unarmedPlatforms.length);
    if (next) {
      setFilter(next);
    }
  });

  // Switching the filter changes the visible set; re-pin to the newest of the new
  // subset so the user always lands on the latest matching event.
  function setFilter(next: DestinationSelection): void {
    filter = next;
    feed.restick();
  }

  function clear(): void {
    // The host clears its store then emits an empty events.backfill; setFeed([])
    // below is a local echo so the feed empties immediately even if the push lags.
    // The echo makes a rejected clear look like it worked, so surface the failure.
    void callOrToast("events.clear", undefined, "Clear events failed");
    feed.setFeed([], true);
  }

  $effect(() => {
    obs
      .call("events.list")
      // events.list / events.backfill arrive newest-first; setFeed reverses into
      // oldest->newest (top->bottom) to match the enqueue-at-bottom order.
      .then((list) => feed.setFeed(list, true))
      .catch(() => {});
    const offNew = obs.on(EV.eventsNew, (e) => feed.enqueue(e));
    const offBackfill = obs.on(EV.eventsBackfill, (batch) => feed.setFeed(batch, true));
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
  {#if destinations.length + unarmedPlatforms.length > 0}
    <div class="bar">
      <DestinationChips
        {destinations}
        value={filter}
        onSelect={setFilter}
        {unarmedPlatforms}
        {unarmedHint}
        {statusOf}
        titleOf={(d, canvas) => "Only events from " + d.displayName + (canvas ? " · " + canvas : "")}
      />
      {#if sharedNotice}
        <p class="notice" title={FIDELITY_HINT.wide}>{sharedNotice}</p>
      {/if}
    </div>
  {/if}

  <div class="scroll" use:feedScroll>
    {#if connectedPlatforms.length === 0}
      <EmptyState compact title="Connect an account to see events." />
    {:else if feed.rows.length === 0}
      <EmptyState compact title="Follows, subs, gifts and cheers from your connected accounts appear here." />
    {:else if filtered.length === 0}
      <EmptyState compact title={filterLabel ? "No events yet for " + filterLabel + "." : "No events yet."} />
    {:else}
      <div class="sizer" style:height={feed.layout.total + "px"}>
        {#each feed.visible as row (row.clientKey)}
          {@const e = row.item}
          {@const actorColor = e.actorColor || PLATFORM_COLOR[e.platform] || "var(--color-muted)"}
          {@const accent = TYPE_COLOR[e.type] ?? "var(--color-muted)"}
          <div class="row selectable" style:top={row.top + "px"} use:measureRow={row.clientKey}>
            <div class="line">
              <span class="pmark"><PlatformMark platform={e.platform} size={12} /></span>
              <span class="icon" style:color={accent} title={TYPE_LABEL[e.type] ?? e.type}
                >{@render typeIcon(e.type)}</span
              >
              <span class="actor" style:color={actorColor}>{e.actorName}</span>
              <span class="sum">{summary(e)}</span>
            </div>
            {#if e.message}<span class="msg">{e.message}</span>{/if}
            {#if multiOrigin}
              {@const o = attribute(e, destByAccount)}
              <!-- "none" means nothing is known about the origin at all: ABSENT_LABEL is
                   documented as an absence it must not guess at, so the row renders
                   nothing rather than a pair of dashes standing in for content. -->
              {#if o.fidelity !== "none"}
                <div class="origin" title={FIDELITY_HINT[o.fidelity]}>
                  <Avatar url={o.avatarUrl} name={o.channel} size={14} />
                  <span class="ochannel">{o.channel}</span>
                  <span class="osep" aria-hidden="true">›</span>
                  <span class="ocanvas" class:state={!o.named}>{o.canvasLabel}</span>
                </div>
              {/if}
            {/if}
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
    flex-direction: column;
    gap: 5px;
    padding: 6px 8px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
  }
  .notice {
    margin: 0;
    font-size: 9.5px;
    line-height: 1.4;
    color: var(--color-dim);
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
  .pmark {
    align-self: center;
    flex: 0 0 auto;
    display: inline-flex;
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
    padding-left: 31px;
    color: var(--color-text);
    overflow-wrap: anywhere;
  }
  /* Which destination this event belongs to: avatar carries the channel (two YouTube
     channels share one red mark, so color cannot), the canvas carries the cut. */
  .origin {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 1px 5px;
    margin-top: 2px;
    min-width: 0;
    font-size: 10px;
  }
  .ochannel {
    min-width: 0;
    max-width: 18ch;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    color: var(--color-dim);
  }
  .osep {
    flex: 0 0 auto;
    color: var(--color-muted);
  }
  .ocanvas {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    color: var(--color-text);
    font-weight: 500;
  }
  /* A state word, not a canvas name -- mono and unbolded so "channel-wide" never
     reads as something the user named. */
  .ocanvas.state {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    font-weight: 400;
    color: var(--color-dim);
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
