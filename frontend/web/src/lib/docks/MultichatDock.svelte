<script lang="ts">
  import { obs, type ChatMessage, type ChatSendParams } from "$lib/api/bridge";
  import { EV } from "$lib/utils/eventNames";
  import { PLATFORM_COLORS, PLATFORM_LABELS, platformKey } from "$lib/theme/platformColors";
  import { FeedVirtualizer, type FeedRow } from "$lib/utils/feedVirtualizer.svelte";
  import { callOrToast } from "$lib/utils/callToast";
  import { fmtChatTime, isRealTimestamp } from "$lib/utils/format";
  import { nowTickStore } from "$lib/stores/nowTickStore.svelte";
  import { destinationKey } from "$lib/api/destinationKeys";
  import { CHAT_STATE_NOTE, chatTransportFor, type ChatTransport } from "$lib/ui/destinationHealth";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import Avatar from "$lib/ui/Avatar.svelte";
  import PlatformMark from "$lib/ui/PlatformMark.svelte";
  import DestinationChips, { type DestinationChipStatus } from "$lib/ui/DestinationChips.svelte";
  import {
    ALL_DESTINATIONS,
    attribute,
    destinationsByAccount,
    matchesSelection,
    reconcileSelection,
    selectionLabel,
    unarmedHint as unarmedHintFor,
    unarmedPlatforms as unarmedPlatformsOf,
    type Attribution,
    type DestinationSelection,
    type Fidelity,
  } from "$lib/ui/destinationSelection";
  import { oauthStore } from "$lib/stores/oauthStore.svelte";
  import { destinationIdentityStore, type DestinationIdentity } from "$lib/stores/destinationIdentityStore.svelte";
  import { transportHealthStore } from "$lib/stores/transportHealthStore.svelte";

  // Host supplies tab chrome + strips __* keys; this body declares no props.
  let {}: Record<string, unknown> = $props();

  const PLATFORM_COLOR = PLATFORM_COLORS;
  const PLATFORM_LABEL = PLATFORM_LABELS;

  // Merged, ring-capped, virtualized scrollback. Rows carry a client-assigned key
  // (m.id could arrive empty/duplicated); 30px estimate for an unmeasured row.
  // Filtering feeds it a derived subset rather than trimming the ring, so a row
  // filtered out keeps its measured height and its place in the 500-row cap.
  const feed = new FeedVirtualizer<ChatMessage>({ max: 500, estimate: 30, getDisplay: () => filtered });
  const measureRow = feed.measureRow;
  const feedScroll = feed.scroll;

  $effect(() => {
    destinationIdentityStore.start();
    const offHealth = transportHealthStore.subscribe();
    const offOauth = oauthStore.subscribe();
    return () => {
      offHealth();
      offOauth();
    };
  });

  // Only an account-backed profile can run a chat transport, so a key/RTMP/WHIP
  // profile could never own a message or receive a reply.
  let destinations = $derived(destinationIdentityStore.all.filter((d) => d.accountId !== ""));
  let destByUuid = $derived(new Map(destinations.map((d) => [d.profileUuid, d])));

  // Needed twice: to attribute a channel-wide message to the streams it could belong
  // to, and to know whether one chat is shared.
  let destByAccount = $derived(destinationsByAccount(destinations));

  // A connected account with no destination still has nothing to read or reply to, so
  // it gets a disabled chip that says why rather than no chip at all.
  let unarmedPlatforms = $derived(unarmedPlatformsOf(oauthStore.connectedPlatforms, destinations));

  function unarmedHint(platform: string): string {
    return unarmedHintFor(platform, "it has no chat here.");
  }

  // --- transport resolution --------------------------------------------------
  // Resolution and wording both live in ui/destinationHealth.ts, shared with the Stats dock's
  // per-output chat line so the two surfaces cannot describe one transport differently.

  const NO_TRANSPORT_NOTE = "no chat transport — nothing to read or reply to here";
  const SHARED_CHAT_NOTE = "one chat for the whole channel, shared with its other streams";

  /** True when this destination's chat is one channel-wide chat that its sibling
   * destinations read and reply to as well. */
  function sharesChat(d: DestinationIdentity, t: ChatTransport): boolean {
    return t.profileUuid === null && (destByAccount.get(d.accountId)?.length ?? 1) >= 2;
  }

  // Absence of a row is UNKNOWN, not healthy: no state (so the chip's edge claims
  // none) and the chip goes unavailable, because there is neither scrollback to filter
  // to nor a transport to reply through.
  function statusOf(d: DestinationIdentity): DestinationChipStatus {
    const t = chatTransportFor(d);
    if (!t) {
      return { note: NO_TRANSPORT_NOTE, unavailable: true };
    }
    const parts = [CHAT_STATE_NOTE[t.row.state]];
    if (t.row.lastError) {
      parts.push(t.row.lastError);
    }
    if (sharesChat(d, t)) {
      parts.push(SHARED_CHAT_NOTE);
    }
    return { state: t.row.state, note: parts.join(" — ") };
  }

  // --- selection: the filter AND the send target -----------------------------
  // One control for both. Today's split (the strip picked where to send while the feed
  // showed everything) is how a reply lands in the wrong chat; bound together, "what am
  // I reading" and "where does this go" cannot disagree.
  let selection = $state<DestinationSelection>(ALL_DESTINATIONS);

  function select(next: DestinationSelection): void {
    selection = next;
    // Switching scope changes the visible set; re-pin to the newest of the new subset.
    feed.restick();
  }

  // Keep the selection valid as destinations come and go.
  $effect(() => {
    const next = reconcileSelection(selection, destinations, destByUuid, unarmedPlatforms.length);
    if (next) {
      select(next);
    }
  });

  // `destination` is the one selection kind that names a single chat, and the only one
  // the composer addresses by accountId.
  let target = $derived(selection.kind === "destination" ? (destByUuid.get(selection.profileUuid) ?? null) : null);
  let targetTransport = $derived(target ? chatTransportFor(target) : null);

  // Explicitly typed to break the feed <-> filtered inference cycle (getDisplay closes
  // over filtered, which reads feed.rows).
  let filtered: FeedRow<ChatMessage>[] = $derived(
    selection.kind === "all" ? feed.rows : feed.rows.filter((r) => matchesSelection(r.item, selection, destByUuid)),
  );

  // --- per-message origin ----------------------------------------------------
  // The tiering is shared with Events (ui/destinationSelection.ts, which documents
  // which sources stamp what); the wording below is this dock's. `exact` is empty on
  // purpose: originTitle already prints platform · channel · canvas, so an exact row's
  // signal is the absence of a caveat.
  const FIDELITY_HINT: Record<Fidelity, string> = {
    exact: "",
    single:
      "Inferred — a channel-wide chat, but this channel has exactly one armed destination, " +
      "so there is no other stream it could belong to.",
    wide: "Channel-wide — this chat is the channel's, not one broadcast's. Naming a canvas here would be a guess.",
    pending: "This destination's canvas has not loaded yet, or was deleted.",
    none: "No stream profile is configured for the account this message came from.",
  };

  function originTitle(m: ChatMessage, o: Attribution): string {
    const platform = PLATFORM_LABEL[platformKey(m.platform)] ?? m.platform;
    const hint = FIDELITY_HINT[o.fidelity];
    return [platform, o.channel, o.canvasLabel].join(" · ") + (hint ? " — " + hint : "");
  }

  // More than one place a message can come from: the point at which every row has to
  // say which destination it belongs to.
  let multiOrigin = $derived(destinations.length + unarmedPlatforms.length >= 2);

  // --- badges ---------------------------------------------------------------
  // "broadcaster" is a badge KIND string rendered generically, so a kind-to-mark map
  // replaces the word without touching the badge pipeline. An unmapped kind keeps
  // rendering as its label, so a new platform badge degrades to text instead of
  // vanishing. Adding one is a single entry.
  const BADGE_MARKS: Record<string, { label: string; d: string }> = {
    broadcaster: { label: "Broadcaster", d: "M3 18h18v3H3zM3 6l4.5 4L12 3l4.5 7L21 6v9H3z" },
  };

  // --- header: which chat am I looking at ------------------------------------
  let scopeLabel = $derived(
    selectionLabel(selection, destByUuid, {
      separator: " › ",
      all: destinations.length >= 2 ? "All " + destinations.length + " chats" : "",
    }),
  );

  // --- empty-state copy -------------------------------------------------------
  // Two situations, not one: "not live" (nothing to have a chat yet) and "live but
  // quiet" (connected, nobody's said anything -- whether that's the whole feed or one
  // scoped destination). Held per state entry, not re-rolled per render or on a timer:
  // this dock sits in a broadcaster's peripheral vision, and a text change there reads
  // as "a message arrived" even when nothing did.

  const OFFLINE_MESSAGES = [
    "Chat shows up once you're live.",
    "Nothing to read yet — go live and it fills in.",
    "This pane wakes up when the stream starts.",
    "No broadcast, no chat.",
    "Idle for now. Chat arrives with the stream.",
    "Empty until you go live.",
    "Standing by for a stream to attach to.",
    "Quiet, because there's nothing on air yet.",
  ];

  const LIVE_EMPTY_MESSAGES = [
    "Quiet in here so far.",
    "No messages yet — this updates the moment one lands.",
    "Nothing said yet. It'll show up here first.",
    "Connected. Just waiting on the first line.",
    "Still quiet. The connection's fine.",
    "Nobody's typed anything yet.",
    "Waiting on the first message.",
    "Connected and listening. Nothing yet.",
  ];

  function pickOne<T>(pool: readonly T[]): T {
    return pool[Math.floor(Math.random() * pool.length)];
  }

  type EmptyKind = "offline" | "live" | null;

  // feed.rows empty with no destinations = not live; feed.rows empty with destinations,
  // or a scoped `filtered` empty while the wider feed isn't, are both "live but quiet" --
  // the scoped case just names which pane via `sub` rather than getting its own tone.
  let emptyKind = $derived<EmptyKind>(
    feed.rows.length === 0
      ? destinations.length > 0
        ? "live"
        : "offline"
      : filtered.length === 0
        ? "live"
        : null,
  );

  let offlineMessage = $state(OFFLINE_MESSAGES[0]);
  let liveEmptyMessage = $state(LIVE_EMPTY_MESSAGES[0]);

  // Reads only the derived `emptyKind`, whose value is stable across renders that don't
  // actually change which of the three branches is showing (Svelte skips an effect
  // rerun when a $derived it reads recomputes to an equal value) -- so this cannot
  // re-roll on an unrelated reactive update, only on a genuine transition into a state.
  let lastEmptyKind: EmptyKind = null;
  $effect(() => {
    const kind = emptyKind;
    if (kind === "offline" && lastEmptyKind !== "offline") {
      offlineMessage = pickOne(OFFLINE_MESSAGES);
    } else if (kind === "live" && lastEmptyKind !== "live") {
      liveEmptyMessage = pickOne(LIVE_EMPTY_MESSAGES);
    }
    lastEmptyKind = kind;
  });

  // --- composer -------------------------------------------------------------
  // Two ways to address a send, and which one is in force is stated, never inferred. A
  // `destination` selection names one transport and is addressed by accountId. `all`
  // and `platform` fan out through the host's `platforms` path -- announcing something
  // to every chat is a normal multistream action, so they stay enabled. What was wrong
  // before was a composer whose target silently disagreed with the feed it sat under;
  // broadcasting is fine as long as every affordance says how many chats it will hit.

  /** Destinations the current selection would fan out to; empty when it names one. */
  let fanDestinations = $derived.by(() => {
    const sel = selection;
    if (sel.kind === "destination") {
      return [];
    }
    if (sel.kind === "platform") {
      return destinations.filter((d) => platformKey(d.platform) === sel.platform);
    }
    return destinations;
  });

  // Distinct CONNECTED transports, each resolved by exact id. Two profiles sharing one
  // account-wide chat are ONE transport, and a destination whose transport has no
  // health row is not connected -- counting destinations, or counting health rows,
  // would promise a fan-out wider than the host will actually perform.
  //
  // `onlyUnavailable` answers WHY the set came out empty, which the size alone cannot:
  // a scope whose every candidate is unavailable has nothing wrong with it, while one
  // holding a single failed or unrowed transport does. The two are counted on this walk
  // rather than a second one, so the composer cannot classify a scope the fan-out never saw.
  let fanScope = $derived.by(() => {
    const connected = new Set<string>();
    let unavailable = 0;
    let unwell = 0;
    for (const d of fanDestinations) {
      const t = chatTransportFor(d);
      if (t && t.row.state === "connected") {
        connected.add(t.id);
      } else if (t && t.row.state === "unavailable") {
        unavailable++;
      } else {
        unwell++;
      }
    }
    return { connected, onlyUnavailable: unavailable > 0 && unwell === 0 };
  });

  /** "3 chats" / "1 YouTube chat" -- one phrase for the band, the button and the
   * placeholder, so the three can never quote different counts. */
  function chatsPhrase(n: number, platform: string): string {
    const label = platform ? (PLATFORM_LABEL[platform] ?? platform) + " " : "";
    return n + " " + label + (n === 1 ? "chat" : "chats");
  }

  const PICK_ONE = "Pick one stream —";
  // The composer's half of the neutral state, worded to describe the broadcast rather than
  // a connection: every scope that reaches it reaches it for the same reason.
  const NO_CHAT_HERE = "No chat to reply to here";

  interface Composer {
    /** The send addressing minus the text; null blocks the composer outright. */
    address: Omit<ChatSendParams, "text"> | null;
    tone: "calm" | "warn" | "fan";
    lead: string;
    to: string;
    button: string;
    buttonTitle: string;
    placeholder: string;
  }

  // `tone` is required rather than defaulted: whether a composer that cannot send is
  // reporting a fault or just describing the situation is a judgement about that branch,
  // and a default answers it for every branch written afterwards without being asked.
  function blocked(lead: string, to: string, placeholder: string, tone: Composer["tone"]): Composer {
    return { address: null, tone, lead, to, button: "Send", buttonTitle: "", placeholder };
  }

  let composer = $derived.by<Composer>(() => {
    const sel = selection;
    if (destinations.length === 0) {
      // Nothing to pick yet, so this is not a warning -- it is the offline state.
      return {
        address: null,
        tone: "calm",
        lead: "No chat while offline",
        to: "",
        button: "Send",
        buttonTitle: "",
        placeholder: "Go live to chat",
      };
    }
    if (sel.kind === "destination") {
      const d = target;
      const t = targetTransport;
      if (!d) {
        return blocked(PICK_ONE, scopeLabel, "Select one stream below to reply…", "warn");
      }
      if (!t) {
        // No transport row at all is UNKNOWN, not healthy: there is nothing to reply
        // through, so this blocks rather than optimistically sending.
        return blocked("No chat transport for", scopeLabel, "This chat is not connected", "warn");
      }
      if (t.row.state === "unavailable") {
        // Nothing was attempted and nothing failed, so this is not a warning -- it is the
        // same reading as the offline state above. Both halves have to hold that line: an
        // amber band saying "not connected" would put back the alarm the neutral edge and
        // the note next to it just took away.
        return blocked(CHAT_STATE_NOTE.unavailable + " —", scopeLabel, NO_CHAT_HERE, "calm");
      }
      if (t.row.state !== "connected") {
        return blocked(CHAT_STATE_NOTE[t.row.state] + " —", scopeLabel, "This chat is not connected", "warn");
      }
      // accountId + profileUuid, never `platforms`: the host routes an accountId to
      // exactly one transport, and a null profileUuid means that account's
      // channel-wide chat. A reply must never reach a second channel.
      return {
        address: { accountId: d.accountId, profileUuid: t.profileUuid },
        tone: "calm",
        lead: sharesChat(d, t) ? "Replying to the whole channel" : "Replying to",
        to: scopeLabel,
        button: "Send",
        buttonTitle: "",
        placeholder: "Message " + scopeLabel + "…",
      };
    }
    const platform = sel.kind === "platform" ? sel.platform : "";
    if (fanScope.connected.size === 0) {
      // This is the view the dock opens on, so the empty scope has to say why it is empty
      // rather than assert the worst of the two reasons. Calm ONLY when every candidate is
      // unavailable: nothing was attempted there, so nothing can be reported as not
      // connected. A mix keeps the warning -- one destination genuinely down among several
      // that were never going to run is still a destination the streamer has to see, and
      // averaging it into the calm reading is how it would go unnoticed.
      if (fanScope.onlyUnavailable) {
        return platform
          ? blocked("No chat on", scopeLabel, NO_CHAT_HERE, "calm")
          : blocked("No chat on this broadcast", "", NO_CHAT_HERE, "calm");
      }
      return platform
        ? blocked("No connected chat on", scopeLabel, "No " + scopeLabel + " chat is connected", "warn")
        : blocked("No chat is connected", "", "No chat is connected", "warn");
    }
    const phrase = chatsPhrase(fanScope.connected.size, platform);
    return {
      // No accountId: this is the host's fan-out path, where an omitted `platforms`
      // means every connected platform -- exactly what `all` asks for.
      address: platform ? { platforms: [platform] } : {},
      tone: "fan",
      lead: "Broadcasting to",
      to: phrase,
      button: "Send to " + phrase,
      buttonTitle: "Post this message to " + phrase + " at once — every one whose chat is currently connected",
      placeholder: "Announce to " + phrase + "…",
    };
  });

  let canSend = $derived(composer.address !== null);

  let draft = $state("");

  function send(): void {
    const text = draft.trim();
    const address = composer.address;
    if (!text || !address) {
      return;
    }
    const params: ChatSendParams = { text, ...address };
    // Clear optimistically; restore the message (if the box is still empty) when the
    // send is rejected so the user doesn't silently lose what they typed.
    draft = "";
    void (async () => {
      const res = await callOrToast("chat.send", params, "Message not sent");
      if (res === null && draft === "") draft = text;
    })();
  }

  function onKeydown(e: KeyboardEvent): void {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      send();
    }
  }

  // The host emits each message once. Chat workers are keyed by destination: one per
  // account on a platform with a single chat per channel, one per live broadcast on a
  // platform that makes a broadcast per stream profile -- so two profiles on one channel
  // are two separate chats whose ids never coincide. Guard the render path anyway: drop a
  // repeated platform-native id so a transport reconnect that replays recent history
  // can't double a line. Keyed by DESTINATION, not platform: two transports on one
  // platform legitimately carry the same platform-native id space.
  const seenIds = new Set<string>();
  const seenOrder: string[] = [];
  function enqueueMessage(m: ChatMessage): void {
    if (m.id) {
      const key = destinationKey(m.accountId, m.profileUuid) + ":" + m.id;
      if (seenIds.has(key)) return;
      seenIds.add(key);
      seenOrder.push(key);
      if (seenOrder.length > 500) {
        const old = seenOrder.shift();
        if (old !== undefined) seenIds.delete(old);
      }
    }
    feed.enqueue(m);
  }

  $effect(() => {
    const offMsg = obs.on(EV.chatMessage, (m) => enqueueMessage(m));
    return () => {
      offMsg();
      feed.dispose();
    };
  });

  // The relative timestamps only need to tick while this pane is actually on screen.
  // Dockview hides an inactive tab by setting the panel's content div to display:none
  // rather than unmounting it, so this component keeps running behind a tab switch --
  // an IntersectionObserver on the dock's own root catches exactly that (it reports
  // not-intersecting for an element with no layout box) without this file needing to
  // know anything about the docking library. The clock's own window-visibility gating
  // lives in nowTickStore; this only decides whether THIS pane holds a ref on it.
  let chatEl: HTMLDivElement | undefined = $state();
  $effect(() => {
    if (!chatEl) return;
    let offTick: (() => void) | null = null;
    const observer = new IntersectionObserver((entries) => {
      // Chromium can batch several records for one target into a single callback when
      // changes accumulate; the most recent one is the one that reflects current state.
      const visible = entries[entries.length - 1]?.isIntersecting ?? false;
      if (visible && !offTick) {
        offTick = nowTickStore.subscribe();
      } else if (!visible && offTick) {
        offTick();
        offTick = null;
      }
    });
    observer.observe(chatEl);
    return () => {
      observer.disconnect();
      offTick?.();
    };
  });
</script>

<div class="chat" bind:this={chatEl}>
  {#if scopeLabel}
    <div class="scopebar" title={"Reading " + scopeLabel}>{scopeLabel}</div>
  {/if}

  <div class="feed">
    <div class="scroll" use:feedScroll>
      {#if feed.rows.length === 0}
        <EmptyState compact title={destinations.length > 0 ? liveEmptyMessage : offlineMessage} />
      {:else if filtered.length === 0}
        <EmptyState compact title={liveEmptyMessage} sub={scopeLabel || undefined} />
      {:else}
        <div class="sizer" style:height={feed.layout.total + "px"}>
          {#each feed.visible as row (row.clientKey)}
            {@const m = row.item}
            {@const authorColor = m.author.color || PLATFORM_COLOR[m.platform]}
            <div
              class="row selectable"
              style:top={row.top + "px"}
              style:border-left-color={PLATFORM_COLOR[m.platform] || "var(--color-muted)"}
              use:measureRow={row.clientKey}
            >
              <!-- Fixed-width leading gutter: the value's own width changes ("now" ->
                   "45m" -> "14:32") as nowTickStore ticks, but the box does not, so
                   later rows never reflow out from under it. -->
              <span class="time" title={isRealTimestamp(m.ts) ? new Date(m.ts).toLocaleString() : ""}
                >{fmtChatTime(m.ts, nowTickStore.nowMs)}</span
              >
              {#if multiOrigin}
                {@const o = attribute(m, destByAccount)}
                <!-- The avatar is what tells two channels of one platform apart: they
                     share one brand mark and one stripe color, so neither can. -->
                <span class="origin" title={originTitle(m, o)}>
                  <PlatformMark platform={m.platform} size={11} />
                  <!-- An unattributable row knows its platform and nothing else. Both the
                       avatar and the canvas label would resolve to ABSENT_LABEL, which
                       prints an absence as if it were content; the mark above is real. -->
                  {#if o.fidelity !== "none"}
                    <Avatar url={o.avatarUrl} name={o.channel} size={15} />
                    {#if !o.named || o.siblings >= 2}
                      <span class="ocanvas" class:state={!o.named}>{o.canvasLabel}</span>
                    {/if}
                  {/if}
                </span>
              {/if}
              {#each m.author.badges as b (b.kind + (b.url ?? ""))}
                {@const mark = BADGE_MARKS[b.kind]}
                {#if b.url}
                  <img class="badge" src={b.url} alt={b.kind} title={b.kind} loading="lazy" draggable="false" />
                {:else if mark}
                  <svg class="badgemark" width="11" height="11" viewBox="0 0 24 24" role="img" aria-label={mark.label}>
                    <path fill="var(--color-accent)" d={mark.d} />
                  </svg>
                {:else}
                  <span class="badgelbl" title={b.kind}>{b.kind}</span>
                {/if}
              {/each}
              <span class="author" style:color={authorColor}>{m.author.name}</span>
              <span class="sep">:</span>
              <span class="text">
                {#each m.fragments as frag, i (i)}
                  {#if frag.type === "text"}{frag.text}{:else}<img
                      class="emote"
                      src={frag.url}
                      alt={frag.code}
                      title={frag.code}
                      loading="lazy"
                      draggable="false"
                    />{/if}
                {/each}
              </span>
            </div>
          {/each}
        </div>
      {/if}
    </div>

    {#if !feed.autoStick && filtered.length > 0}
      <button class="jump" onclick={feed.jumpToLatest}><Icon name="jump-down" size={11} /> Jump to latest</button>
    {/if}
  </div>

  {#if destinations.length + unarmedPlatforms.length > 0}
    <div class="dests">
      <DestinationChips
        {destinations}
        value={selection}
        onSelect={select}
        {unarmedPlatforms}
        {unarmedHint}
        {statusOf}
        titleOf={(d, canvas) => "Read and reply in " + d.displayName + (canvas ? " · " + canvas : "")}
      />
    </div>
  {/if}

  <div class="composer">
    <p class="replyto" class:warn={composer.tone === "warn"} class:fan={composer.tone === "fan"}>
      <span>{composer.lead}</span>
      {#if composer.to}<span class="to">{composer.to}</span>{/if}
    </p>
    <div class="inputrow">
      <textarea
        class="input"
        rows="1"
        bind:value={draft}
        onkeydown={onKeydown}
        disabled={!canSend}
        placeholder={composer.placeholder}
        aria-label="Chat message"
      ></textarea>
      <button
        class="sendbtn"
        disabled={!canSend || draft.trim() === ""}
        title={composer.buttonTitle || undefined}
        onclick={send}>{composer.button}</button
      >
    </div>
  </div>
</div>

<style>
  .chat {
    height: 100%;
    display: flex;
    flex-direction: column;
    background: var(--color-surface);
    font-family: var(--font-ui);
    min-height: 0;
  }
  /* Which chat is on screen, restated in words: a highlighted chip is state you stop
     seeing after an hour. */
  .scopebar {
    flex: 0 0 auto;
    padding: 5px 10px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
    font-family: var(--font-mono);
    font-size: 9.5px;
    letter-spacing: 0.1em;
    text-transform: var(--label-case);
    color: var(--color-dim);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  /* Own containing block for the jump chip, so a wrapping chip strip can't push it
     off the feed the way a fixed offset did. */
  .feed {
    flex: 1;
    min-height: 0;
    position: relative;
    display: flex;
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
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 0 5px;
    padding: 3px 10px 3px 7px;
    border-left: 3px solid transparent;
    font-size: 12px;
    line-height: 1.5;
    color: var(--color-text);
    word-break: break-word;
  }
  /* min-width in ch (the font's own digit width) rather than a px guess -- the three
     shapes fmtChatTime returns ("now", "45m", "14:32") top out at 5 characters, so this
     is exactly wide enough for the longest and never resizes as a row's value grows. */
  .time {
    flex: 0 0 auto;
    align-self: baseline;
    min-width: 5ch;
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
    font-size: 10px;
    color: var(--color-muted);
  }
  .origin {
    align-self: center;
    flex: 0 0 auto;
    display: inline-flex;
    align-items: center;
    gap: 3px;
    min-width: 0;
  }
  .ocanvas {
    min-width: 0;
    max-width: 10ch;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: 10px;
    color: var(--color-dim);
  }
  /* A state word, not a canvas name -- mono and dimmer so "channel-wide" never reads
     as something the user named. */
  .ocanvas.state {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    color: var(--color-muted);
  }
  .badge {
    height: 14px;
    width: auto;
    align-self: center;
    flex: 0 0 auto;
  }
  .badgemark {
    align-self: center;
    flex: 0 0 auto;
    display: block;
  }
  .badgelbl {
    align-self: center;
    flex: 0 0 auto;
    padding: 0 3px;
    font-family: var(--font-mono);
    font-size: 8px;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: var(--color-base);
    background: var(--color-muted);
  }
  .author {
    font-weight: 600;
    overflow-wrap: anywhere;
  }
  .sep {
    color: var(--color-muted);
    margin-left: -3px;
  }
  .text {
    color: var(--color-text);
    overflow-wrap: anywhere;
  }
  .emote {
    height: 18px;
    width: auto;
    vertical-align: middle;
    margin: 0 1px;
  }

  .jump {
    position: absolute;
    left: 50%;
    transform: translateX(-50%);
    bottom: 8px;
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

  /* The selector sits on the base surface, between the feed it filters and the
     composer it aims -- one strip physically touching both things it controls. */
  .dests {
    flex: 0 0 auto;
    padding: 6px 8px;
    border-top: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
  }
  .composer {
    flex: 0 0 auto;
    border-top: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
  }
  .replyto {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 0 5px;
    margin: 0;
    padding: 5px 8px;
    border-bottom: var(--border-weight) solid var(--color-border-2);
    font-family: var(--font-mono);
    font-size: 9.5px;
    letter-spacing: 0.09em;
    text-transform: var(--label-case);
    color: var(--color-muted);
  }
  .replyto .to {
    color: var(--color-accent);
    overflow-wrap: anywhere;
  }
  .replyto.warn {
    background: color-mix(in srgb, var(--color-warn) 9%, transparent);
  }
  .replyto.warn .to {
    color: var(--color-warn);
  }
  /* Broadcasting is not a warning, so it does not borrow the warn color -- but it is
     also not the calm one-reply default, so it gets its own tint and edge. The words
     carry the state; this only makes the band impossible to skim past. */
  .replyto.fan {
    background: color-mix(in srgb, var(--color-accent) 10%, transparent);
    box-shadow: inset 3px 0 0 var(--color-accent);
  }
  .inputrow {
    display: flex;
    align-items: stretch;
    gap: 6px;
    padding: 6px 8px 8px;
  }
  .input {
    flex: 1;
    /* Floor, not 0: the fan-out send label is long enough to crush the box on a narrow
       dock, and the button shrinks first. */
    min-width: 5.5em;
    resize: none;
    padding: 6px 8px;
    font-family: var(--font-ui);
    font-size: 12px;
    line-height: 1.4;
    color: var(--color-text);
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
  }
  .input:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .input:disabled {
    color: var(--color-muted);
    cursor: not-allowed;
  }
  .sendbtn {
    flex: 0 1 auto;
    min-width: 0;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
    padding: 0 14px;
    font-size: 11px;
    font-weight: 600;
    font-family: var(--font-ui);
    color: var(--color-accent-ink);
    background: var(--color-accent);
    border: 0;
    cursor: pointer;
  }
  .sendbtn:disabled {
    opacity: 0.5;
    cursor: default;
  }
</style>
