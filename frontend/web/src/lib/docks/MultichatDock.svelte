<script lang="ts">
  import { obs, type ChatMessage, type ChatState, type ChatPlatform } from "../bridge";
  import { PLATFORM_COLORS, PLATFORM_LABELS, PLATFORM_ORDER as ORDER } from "../theme/platformColors";
  import EmptyState from "../EmptyState.svelte";
  import Icon from "../dock/Icon.svelte";

  // Host supplies tab chrome + strips __* keys; this body declares no props.
  let {}: Record<string, unknown> = $props();

  // Brand colors (spec): author-color fallback + platform dot/tag + dest chips.
  const PLATFORM_COLOR = PLATFORM_COLORS;
  const PLATFORM_LABEL = PLATFORM_LABELS;
  // Stable chip order so the dest selector / connected chips never reshuffle.
  const PLATFORM_ORDER: readonly ChatPlatform[] = ORDER;

  // --- merged scrollback (ring-capped + virtualized) ------------------------
  // Hard cap on retained messages so sustained chat can't grow the array or the
  // measured-height map without bound; trimming prunes both in lockstep.
  const MAX = 500;
  const ESTIMATE = 30; // px height estimate for an unmeasured row
  const OVERSCAN = 6; // rows rendered beyond the viewport on each side
  const STICK_PX = 24; // within this of the bottom counts as "stuck to latest"

  // Each row carries a client-assigned monotonic key so the render key never
  // depends on the host-supplied m.id (which could arrive empty or duplicated and
  // would throw each_key_duplicate). m.id is retained on the message for any
  // host-side identity needs; the keyed list + heights map use clientKey only.
  let messages = $state<{ clientKey: number; m: ChatMessage }[]>([]);
  let seq = 0;
  // clientKey -> measured row height. Plain map (not deep-reactive); a height change
  // bumps `measureVersion` to recompute the layout. Pruned alongside the ring trim.
  const heights = new Map<number, number>();
  let measureVersion = $state(0);

  let scrollEl: HTMLDivElement | undefined;
  let viewTop = $state(0);
  let viewH = $state(0);
  let autoStick = $state(true);

  // Batch incoming messages onto a single rAF flush so a burst re-renders once
  // (not once per message) and the array is rebuilt at most once per frame.
  let pending: ChatMessage[] = [];
  let rafId = 0;
  function enqueue(m: ChatMessage): void {
    pending.push(m);
    if (!rafId) rafId = requestAnimationFrame(flush);
  }
  function flush(): void {
    rafId = 0;
    if (pending.length === 0) return;
    let next = messages.concat(pending.map((m) => ({ clientKey: ++seq, m })));
    pending = [];
    if (next.length > MAX) {
      // clientKeys are unique+monotonic, so trimmed rows never alias a kept one --
      // prune their heights directly, in lockstep with the ring trim.
      for (const d of next.slice(0, next.length - MAX)) heights.delete(d.clientKey);
      next = next.slice(next.length - MAX);
    }
    messages = next;
  }

  // Cumulative row offsets + total height; recomputed when the message set or any
  // measured height changes. O(n) over the <=MAX ring -- cheap.
  let layout = $derived.by<{ tops: number[]; total: number }>(() => {
    void measureVersion;
    const tops = new Array<number>(messages.length);
    let acc = 0;
    for (let i = 0; i < messages.length; i++) {
      tops[i] = acc;
      acc += heights.get(messages[i].clientKey) ?? ESTIMATE;
    }
    return { tops, total: acc };
  });

  // Visible window [start, end) including overscan, from the scroll offset.
  let range = $derived.by<{ start: number; end: number }>(() => {
    const { tops } = layout;
    const n = messages.length;
    if (n === 0) return { start: 0, end: 0 };
    const top = viewTop;
    const bottom = viewTop + viewH;
    let start = n - 1;
    for (let i = 0; i < n; i++) {
      const h = heights.get(messages[i].clientKey) ?? ESTIMATE;
      if (tops[i] + h > top) {
        start = i;
        break;
      }
    }
    let end = n;
    for (let i = start; i < n; i++) {
      if (tops[i] > bottom) {
        end = i;
        break;
      }
    }
    return { start: Math.max(0, start - OVERSCAN), end: Math.min(n, end + OVERSCAN) };
  });

  let visible = $derived(
    messages.slice(range.start, range.end).map((row, i) => ({ ...row, top: layout.tops[range.start + i] })),
  );

  // Measure each rendered row; a height delta bumps measureVersion so the layout
  // (and thus the bottom anchor) reflects real wrapped/emote heights.
  function measureRow(node: HTMLElement, key: number): { destroy(): void } {
    const apply = (): void => {
      const h = node.offsetHeight;
      if (h > 0 && heights.get(key) !== h) {
        heights.set(key, h);
        measureVersion++;
      }
    };
    const ro = new ResizeObserver(apply);
    ro.observe(node);
    apply();
    return {
      destroy() {
        ro.disconnect();
      },
    };
  }

  function onScroll(): void {
    if (!scrollEl) return;
    viewTop = scrollEl.scrollTop;
    viewH = scrollEl.clientHeight;
    autoStick = scrollEl.scrollHeight - scrollEl.scrollTop - scrollEl.clientHeight <= STICK_PX;
  }

  function jumpToLatest(): void {
    autoStick = true;
    if (scrollEl) scrollEl.scrollTop = scrollEl.scrollHeight;
  }

  // Keep the view pinned to the newest message while stuck to the bottom. Depends
  // on layout.total so it re-pins after a freshly measured row grows the sizer.
  $effect(() => {
    void layout.total;
    if (autoStick && scrollEl) {
      scrollEl.scrollTop = scrollEl.scrollHeight;
      // Keep the window state coherent without waiting on the async scroll event.
      viewTop = scrollEl.scrollTop;
    }
  });

  // Track the viewport height (jump-to-latest visibility + range both need it).
  $effect(() => {
    if (!scrollEl) return;
    viewH = scrollEl.clientHeight;
    const ro = new ResizeObserver(() => {
      if (scrollEl) viewH = scrollEl.clientHeight;
    });
    ro.observe(scrollEl);
    return () => ro.disconnect();
  });

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

  // "all" or a single connected platform. Falls back to "all" if the chosen
  // platform disconnects.
  let dest = $state<"all" | ChatPlatform>("all");
  $effect(() => {
    if (dest !== "all" && !connected.includes(dest)) dest = "all";
  });

  let draft = $state("");

  function send(): void {
    const text = draft.trim();
    if (!text || !anyConnected) return;
    const platforms = dest === "all" ? [] : [dest];
    obs.call("chat.send", { platforms, text }).catch(() => {});
    draft = "";
  }

  function onKeydown(e: KeyboardEvent): void {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      send();
    }
  }

  $effect(() => {
    refreshStates();
    const offMsg = obs.on("chat.message", (m) => enqueue(m));
    const offState = obs.on("chat.state", (s) => {
      const next = new Map(states);
      next.set(s.platform, s);
      states = next;
    });
    // No teardown event fires when the host stops the transports on stream-stop,
    // so re-snapshot the (now empty) state set whenever streaming flips.
    const offStreaming = obs.on("streaming.changed", () => refreshStates());
    return () => {
      offMsg();
      offState();
      offStreaming();
      if (rafId) cancelAnimationFrame(rafId);
      rafId = 0;
      pending = [];
    };
  });
</script>

<div class="chat">
  <div class="scroll" bind:this={scrollEl} onscroll={onScroll}>
    {#if messages.length === 0}
      <EmptyState compact title={anyConnected ? "Waiting for chat…" : "Chat appears here while you are live."} />
    {:else}
      <div class="sizer" style:height={layout.total + "px"}>
        {#each visible as row (row.clientKey)}
          {@const m = row.m}
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

  {#if !autoStick && messages.length > 0}
    <button class="jump" onclick={jumpToLatest}><Icon name="jump-down" size={11} /> Jump to latest</button>
  {/if}

  <div class="composer">
    <div class="dests">
      <button class="chip" class:on={dest === "all"} disabled={!anyConnected} onclick={() => (dest = "all")}>
        All
      </button>
      {#each connected as p (p)}
        <button
          class="chip"
          class:on={dest === p}
          style:--chip={PLATFORM_COLOR[p]}
          onclick={() => (dest = p)}
        >
          <span class="cdot" style:background={PLATFORM_COLOR[p]}></span>
          {PLATFORM_LABEL[p]}
        </button>
      {/each}
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
  .chip:disabled {
    opacity: 0.5;
    cursor: default;
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
