<script lang="ts">
  // Live preview of the served widget document inside an <iframe> (the same URL an
  // OBS Browser Source loads), plus a bar of test controls that push a synthetic frame to
  // THIS widget only (overlays.test -> BroadcastTo). The iframe is keyed on reloadKey so
  // the page can force a reload after a debounced update lands.
  //
  // Which controls appear is read from the widget type's `tests` capabilities, which name
  // the SSE channels its template actually subscribes to (see widgetTypes.ts). A widget
  // painting only from onChat has no use for a Follow button, and its preview stays a
  // blank white rectangle until something feeds the channel it does listen to.
  import { type EventType } from "$lib/api/bridge";
  import { testsFor, type TestCapability } from "$lib/overlays/widgetTypes";
  import { EVENT_TYPE_COLORS, EVENT_TYPE_LABELS } from "$lib/theme/platformColors";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import { callOrToast } from "$lib/utils/callToast";

  let {
    url,
    widgetId,
    widgetType,
    reloadKey,
    naturalW,
    naturalH,
  }: {
    url: string;
    widgetId: string;
    widgetType: string;
    reloadKey: number;
    naturalW?: number;
    naturalH?: number;
  } = $props();

  // The document is rendered at the widget type's own design rectangle and the whole frame
  // is then scaled into the pane, rather than stretched to it. A widget's stylesheet reads
  // 100vw/100vh off its viewport and sizes everything from that, so an iframe given the
  // pane's shape previews a rectangle no browser source is ever drawn at -- a tall pane
  // would show a bar meant to be 54px tall filling several hundred.
  // Falls back to filling the pane for a type that reports no rectangle (one this build
  // does not ship), which is what the pane did before and still the only option there.
  let paneW = $state(0);
  let paneH = $state(0);
  const fitted = $derived(
    naturalW && naturalH && paneW > 0 && paneH > 0
      ? { w: naturalW, h: naturalH, scale: Math.min(paneW / naturalW, paneH / naturalH) }
      : null,
  );

  const ALERT_TYPES: EventType[] = [
    "follow",
    "sub",
    "resub",
    "subgift",
    "cheer",
    "raid",
    "superchat",
    "supersticker",
    "member",
  ];

  /** The word each capability contributes to the bar's heading. A capability that only
   * resets the widget names nothing the user is testing, so it contributes none. */
  const CAPABILITY_WORD: Partial<Record<TestCapability, string>> = {
    alerts: "alerts",
    chat: "chat",
    viewers: "viewers",
    channels: "followers",
    stream: "stream",
  };

  // Three lists of pairwise-coprime length (3 · 7 · 8) walked by one counter, so a burst
  // reads as several people talking rather than as the same line five times, and no pair
  // of columns locks into step.
  const CHAT_PLATFORMS = ["twitch", "youtube", "kick"];
  const CHAT_AUTHORS = [
    "nightowl",
    "pixelpanda",
    "verdant_fox",
    "quietstorm",
    "moss_and_ash",
    "lofi_lynx",
    "static_wren",
  ];
  const CHAT_TEXTS = [
    "first",
    "that transition was clean",
    "gg",
    "how did you set that up?",
    "o7",
    "audio sounds way better now",
    "hi from the vod",
    "chat is fast today",
  ];

  const capabilities = $derived(testsFor(widgetType));
  const heading = $derived.by(() => {
    const words = capabilities.map((c) => CAPABILITY_WORD[c]).filter((w): w is string => !!w);
    return words.length > 0 ? "Test " + words.join(" · ") : "Test";
  });

  /** The counter channels: a numeric box and a send button, alike but for these three
   * words, so a third counter is a row here rather than a fourth branch below. */
  const COUNTERS: Partial<Record<TestCapability, { channel: string; inputLabel: string; button: string }>> = {
    viewers: { channel: "viewers", inputLabel: "Test viewer count", button: "Set viewers" },
    channels: { channel: "channels", inputLabel: "Test follower count", button: "Set followers" },
  };

  // Seeded with plausible figures rather than zeros, so one click paints something.
  let counts = $state<Partial<Record<TestCapability, number>>>({ viewers: 128, channels: 1240 });
  let chatSeq = 0;

  // bind:value on a number input writes undefined once the box is cleared, and an empty
  // box has to mean zero rather than "unset": JSON.stringify drops an undefined value and
  // the host fills a missing count with a stand-in figure of its own, so clearing the field
  // would paint a number nobody asked for. Zero is a real audience count and one the
  // widgets have to be able to show. A negative is clamped rather than sent — `min="0"`
  // only marks the input invalid, it does not stop the value being read back.
  function asCount(n: number | undefined): number {
    return Number.isFinite(n) ? Math.max(0, n as number) : 0;
  }

  // The iframe below subscribes to this widget's SSE stream, so once it has loaded it is
  // itself a listener and `delivered` should never be zero. A zero therefore means the
  // frame reached nobody — the state that is otherwise indistinguishable from a widget
  // that received it and drew nothing, which is the confusion this whole bar exists to
  // remove.
  //
  // Gated on the frame having loaded, and on the load belonging to the CURRENT reloadKey:
  // {#key reloadKey} tears the iframe down and rebuilds it after every save, and changing
  // a value then testing it is the normal way to use this bar, so an ungated zero would
  // fire most often in the one moment it means nothing. `load` waits on the document's
  // classic <script src="/runtime.js">, which opens the EventSource as it runs, so a
  // loaded frame has its subscription in flight.
  let loadedKey = $state(-1);
  const previewListening = $derived(loadedKey === reloadKey);

  // Rejections are toasted rather than logged: these are direct user actions, and a button
  // that failed outright would otherwise look exactly like one that worked.
  function fire(params: Record<string, unknown>): void {
    void callOrToast("overlays.test", { id: widgetId, ...params }, "Test failed").then((r) => {
      if (r && r.delivered === 0 && previewListening) {
        showToast(
          "Test sent, but nothing received it.",
          "This overlay has no live listener — neither the preview nor any Browser Source is connected.",
        );
      }
    });
  }

  function sendChat(): void {
    const i = chatSeq++;
    fire({
      channel: "chat",
      overrides: {
        platform: CHAT_PLATFORMS[i % CHAT_PLATFORMS.length],
        author: CHAT_AUTHORS[i % CHAT_AUTHORS.length],
        text: CHAT_TEXTS[i % CHAT_TEXTS.length],
      },
    });
  }

  function burstChat(): void {
    for (let n = 0; n < 5; n++) {
      sendChat();
    }
  }

  // Clear and End stream are the same frame under the two names a widget earns: chatbox,
  // chat leaderboard and viewer count each empty themselves on a stream frame whose
  // `active` is not true, which is the only clear path their templates have. The host
  // stamps `startedAt` on a live frame itself, off the same wall clock this page reads, so
  // Stream Uptime has a start to count from.
  function setStreamActive(active: boolean): void {
    fire({ channel: "stream", overrides: { active } });
  }
</script>

<div class="preview">
  {#if capabilities.length > 0}
    <div class="test-bar">
      <span class="kicker">{heading}</span>
      <div class="test-row" role="group" aria-label={heading}>
        {#each capabilities as cap (cap)}
          {@const counter = COUNTERS[cap]}
          {#if cap === "alerts"}
            {#each ALERT_TYPES as t (t)}
              <button class="test-btn" style:--dot={EVENT_TYPE_COLORS[t]} onclick={() => fire({ type: t })}>
                <span class="dot"></span>{EVENT_TYPE_LABELS[t]}
              </button>
            {/each}
          {:else if cap === "chat"}
            <button class="test-btn" onclick={sendChat}>Send message</button>
            <button class="test-btn" onclick={burstChat}>Burst ×5</button>
          {:else if counter}
            <input class="test-in" type="number" min="0" aria-label={counter.inputLabel} bind:value={counts[cap]} />
            <button
              class="test-btn"
              onclick={() => fire({ channel: counter.channel, overrides: { count: asCount(counts[cap]) } })}
            >
              {counter.button}
            </button>
          {:else if cap === "stream"}
            <button class="test-btn" onclick={() => setStreamActive(true)}>Go live</button>
            <button class="test-btn" onclick={() => setStreamActive(false)}>End stream</button>
          {:else if cap === "clear"}
            <button class="test-btn" onclick={() => setStreamActive(false)}>Clear</button>
          {/if}
        {/each}
      </div>
    </div>
  {/if}
  <div class="frame" bind:clientWidth={paneW} bind:clientHeight={paneH}>
    {#key reloadKey}
      <iframe
        class:fitted={!!fitted}
        title="Overlay preview"
        src={url}
        sandbox="allow-scripts allow-same-origin"
        onload={() => (loadedKey = reloadKey)}
        style={fitted ? `width:${fitted.w}px;height:${fitted.h}px;--preview-scale:${fitted.scale}` : ""}
      ></iframe>
    {/key}
  </div>
</div>

<style>
  .preview {
    display: flex;
    flex-direction: column;
    height: 100%;
    min-height: 0;
  }
  .test-bar {
    flex: 0 0 auto;
    display: flex;
    flex-direction: column;
    gap: 8px;
    padding: 0 0 12px;
  }
  .kicker {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .test-row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px;
  }
  .test-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 5px 10px;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    cursor: pointer;
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.04em;
    text-transform: uppercase;
  }
  .test-btn:hover {
    color: var(--color-text);
    border-color: var(--color-accent);
  }
  /* Matches the buttons it sits between rather than the page's 34px .cv-num, which would
     tower over a bar built at button height. */
  .test-in {
    width: 76px;
    padding: 5px 8px;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-mono);
    font-size: 10px;
    text-align: right;
  }
  .test-in:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .dot {
    width: 7px;
    height: 7px;
    flex: 0 0 auto;
    background: var(--dot);
  }
  .frame {
    flex: 1;
    min-height: 0;
    /* A scaled iframe still occupies its unscaled layout box, so the pane positions it
       itself and clips whatever the design rectangle leaves outside. */
    position: relative;
    overflow: hidden;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
  }
  iframe {
    display: block;
    width: 100%;
    height: 100%;
    border: 0;
    background: var(--color-base);
  }
  /* Sized in the markup to the widget's design rectangle; --preview-scale is what fits
     that rectangle into the pane. */
  iframe.fitted {
    position: absolute;
    left: 50%;
    top: 50%;
    transform: translate(-50%, -50%) scale(var(--preview-scale));
  }
</style>
