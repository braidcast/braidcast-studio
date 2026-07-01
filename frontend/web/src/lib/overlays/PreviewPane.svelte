<script lang="ts">
  // Live preview of the served widget document inside an <iframe> (the same URL an
  // OBS Browser Source loads), plus a row of per-event-type test buttons that fire a
  // synthetic event to THIS widget only (overlays.test -> BroadcastTo). The iframe is
  // keyed on reloadKey so the page can force a reload after a debounced update lands.
  // TYPE_LABEL/TYPE_COLOR are re-declared here (not imported) to stay decoupled from
  // the EventsDock — they are local consts there.
  import { obs, type EventType } from "../bridge";

  let { url, widgetId, reloadKey }: { url: string; widgetId: string; reloadKey: number } = $props();

  const TEST_TYPES: EventType[] = [
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
  const TYPE_LABEL: Record<EventType, string> = {
    follow: "Follow",
    sub: "Sub",
    resub: "Resub",
    subgift: "Gift Sub",
    cheer: "Cheer",
    raid: "Raid",
    superchat: "Super Chat",
    supersticker: "Super Sticker",
    member: "Member",
  };
  const TYPE_COLOR: Record<EventType, string> = {
    follow: "#3ea6ff",
    sub: "#a970ff",
    resub: "#a970ff",
    subgift: "#ffb62c",
    cheer: "#1fd1c3",
    raid: "#ff8a3d",
    superchat: "#2ecc71",
    supersticker: "#2ecc71",
    member: "#ffb62c",
  };

  function test(type: EventType): void {
    obs
      .call("overlays.test", { id: widgetId, type })
      .catch((e) => console.log("overlays.test failed: " + (e as Error).message));
  }
</script>

<div class="preview">
  <div class="test-bar">
    <span class="kicker">Test alerts</span>
    <div class="test-row">
      {#each TEST_TYPES as t (t)}
        <button class="test-btn" style:--dot={TYPE_COLOR[t]} onclick={() => test(t)}>
          <span class="dot"></span>{TYPE_LABEL[t]}
        </button>
      {/each}
    </div>
  </div>
  <div class="frame">
    {#key reloadKey}
      <iframe title="Overlay preview" src={url} sandbox="allow-scripts allow-same-origin"></iframe>
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
  .dot {
    width: 7px;
    height: 7px;
    flex: 0 0 auto;
    background: var(--dot);
  }
  .frame {
    flex: 1;
    min-height: 0;
    border: var(--border-weight) solid var(--color-border);
    background: #101014;
  }
  iframe {
    display: block;
    width: 100%;
    height: 100%;
    border: 0;
    background: #101014;
  }
</style>
