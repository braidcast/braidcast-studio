<script lang="ts">
  import { obs, type ChatMessage, type ChatState, type ChatPlatform } from "../bridge";
import { EV } from "../eventNames";
  import { PLATFORM_COLORS, PLATFORM_LABELS, PLATFORM_ORDER as ORDER } from "../theme/platformColors";
  import { FeedVirtualizer } from "../feedVirtualizer.svelte";
  import { callOrToast } from "../callToast";
  import EmptyState from "../EmptyState.svelte";
  import Icon from "../dock/Icon.svelte";
  import PlatformChips from "../PlatformChips.svelte";

  // Host supplies tab chrome + strips __* keys; this body declares no props.
  let {}: Record<string, unknown> = $props();

  // Brand colors (spec): author-color fallback + platform dot/tag + dest chips.
  const PLATFORM_COLOR = PLATFORM_COLORS;
  const PLATFORM_LABEL = PLATFORM_LABELS;
  // Stable chip order so the dest selector / connected chips never reshuffle.
  const PLATFORM_ORDER: readonly ChatPlatform[] = ORDER;

  // Merged, ring-capped, virtualized scrollback. Rows carry a client-assigned key
  // (m.id could arrive empty/duplicated); 30px estimate for an unmeasured row.
  const feed = new FeedVirtualizer<ChatMessage>({ max: 500, estimate: 30 });
  const measureRow = feed.measureRow;
  const feedScroll = feed.scroll;

  // --- connection state + send destination ----------------------------------
  // platform -> latest ChatState. The chat.state METHOD returns the full array
  // (snapshot); the chat.state EVENT reports ONE platform, merged here by key.
  let states = $state<Map<ChatPlatform, ChatState>>(new Map());

  function refreshStates(): void {
    obs
      .call("chat.state")
      .then((rows) => {
        const next = new Map<ChatPlatform, ChatState>();
        for (const r of rows) next.set(r.platform, r);
        states = next;
      })
      .catch(() => {});
  }

  let connected = $derived(PLATFORM_ORDER.filter((p) => states.get(p)?.connected === true));
  let anyConnected = $derived(connected.length > 0);

  // "all" or a single connected platform. The "All" dest (and its chip) is only
  // meaningful with >=2 connected platforms; with exactly one, pin the dest to it so
  // its lone chip reads active. Falls back to "all" if the chosen platform disconnects.
  let dest = $state<"all" | ChatPlatform>("all");
  let showAllChip = $derived(connected.length >= 2);
  $effect(() => {
    if (connected.length === 1) {
      if (dest !== connected[0]) dest = connected[0];
    } else if (dest !== "all" && !connected.includes(dest)) {
      dest = "all";
    }
  });

  let draft = $state("");

  function send(): void {
    const text = draft.trim();
    if (!text || !anyConnected) return;
    const platforms = dest === "all" ? [] : [dest];
    // Clear optimistically; restore the message (if the box is still empty) when the
    // send is rejected so the user doesn't silently lose what they typed.
    draft = "";
    void (async () => {
      const res = await callOrToast("chat.send", { platforms, text }, "Message not sent");
      if (res === null && draft === "") draft = text;
    })();
  }

  function onKeydown(e: KeyboardEvent): void {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      send();
    }
  }

  // The host emits each message once (chat workers are keyed by account, so two
  // profiles on one channel share a worker). Guard the render path anyway: drop a
  // repeated platform-native id so a transport reconnect that replays recent
  // history can't double a line. Bounded ring; ids are optional per platform.
  const seenIds = new Set<string>();
  const seenOrder: string[] = [];
  function enqueueMessage(m: ChatMessage): void {
    if (m.id) {
      const key = m.platform + ":" + m.id;
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
    refreshStates();
    const offMsg = obs.on(EV.chatMessage, (m) => enqueueMessage(m));
    const offState = obs.on(EV.chatState, (s) => {
      const next = new Map(states);
      next.set(s.platform, s);
      states = next;
    });
    // No teardown event fires when the host stops the transports on stream-stop,
    // so re-snapshot the (now empty) state set whenever streaming flips.
    const offStreaming = obs.on(EV.streamingChanged, () => refreshStates());
    return () => {
      offMsg();
      offState();
      offStreaming();
      feed.dispose();
    };
  });
</script>

<div class="chat">
  <div class="scroll" use:feedScroll>
    {#if feed.rows.length === 0}
      <EmptyState compact title={anyConnected ? "Waiting for chat…" : "Chat appears here while you are live."} />
    {:else}
      <div class="sizer" style:height={feed.layout.total + "px"}>
        {#each feed.visible as row (row.clientKey)}
          {@const m = row.item}
          {@const authorColor = m.author.color || PLATFORM_COLOR[m.platform]}
          <div class="row" style:top={row.top + "px"} use:measureRow={row.clientKey}>
            <span class="pdot" style:background={PLATFORM_COLOR[m.platform]} title={PLATFORM_LABEL[m.platform]}
            ></span>
            {#each m.author.badges as b (b.kind + (b.url ?? ""))}
              {#if b.url}
                <img class="badge" src={b.url} alt={b.kind} title={b.kind} loading="lazy" draggable="false" />
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

  {#if !feed.autoStick && feed.rows.length > 0}
    <button class="jump" onclick={feed.jumpToLatest}><Icon name="jump-down" size={11} /> Jump to latest</button>
  {/if}

  <div class="composer">
    <div class="dests">
      <PlatformChips platforms={connected} value={dest} showAll={showAllChip} onSelect={(v) => (dest = v)} />
    </div>
    <div class="inputrow">
      <textarea
        class="input"
        rows="1"
        bind:value={draft}
        onkeydown={onKeydown}
        disabled={!anyConnected}
        placeholder={anyConnected
          ? "Message " + (dest === "all" ? "all platforms" : PLATFORM_LABEL[dest as ChatPlatform]) + "…"
          : states.size > 0
            ? "No connected accounts"
            : "Go live to chat"}
        aria-label="Chat message"
      ></textarea>
      <button class="sendbtn" disabled={!anyConnected || draft.trim() === ""} onclick={send}>Send</button>
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
    position: relative;
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
    padding: 3px 10px;
    font-size: 12px;
    line-height: 1.5;
    color: var(--color-text);
    word-break: break-word;
  }
  .pdot {
    align-self: center;
    width: 7px;
    height: 7px;
    flex: 0 0 auto;
  }
  .badge {
    height: 14px;
    width: auto;
    align-self: center;
    flex: 0 0 auto;
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
    bottom: 78px;
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

  .composer {
    flex: 0 0 auto;
    border-top: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
  }
  .dests {
    display: flex;
    flex-wrap: wrap;
    gap: 4px;
    padding: 6px 8px 0;
  }
  .inputrow {
    display: flex;
    align-items: stretch;
    gap: 6px;
    padding: 6px 8px 8px;
  }
  .input {
    flex: 1;
    min-width: 0;
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
    flex: 0 0 auto;
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
