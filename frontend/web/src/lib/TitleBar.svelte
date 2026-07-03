<script lang="ts">
  import { onMount } from "svelte";
  import { obs } from "./bridge";
  import Icon from "./dock/Icon.svelte";
  import { WINDOW_ID } from "./windowContext";

  interface Props {
    // "detached" folds the torn-out window's title + REDOCK into the strip.
    variant?: "main" | "detached";
    title?: string;
    onRedock?: () => void;
  }
  let { variant = "main", title = "", onRedock }: Props = $props();

  let maximized = $state(false);

  // The native host pushes window.stateChanged on maximize/restore so the glyph can
  // toggle; each window filters to its own id (main = 0).
  onMount(() =>
    obs.on("window.stateChanged", (p) => {
      if (p.windowId === WINDOW_ID) maximized = p.maximized;
    }),
  );

  const minimize = () => void obs.call("window.minimize", { windowId: WINDOW_ID }).catch(() => {});
  const toggleMax = () => void obs.call("window.toggleMaximize", { windowId: WINDOW_ID }).catch(() => {});
  const close = () => void obs.call("window.close", { windowId: WINDOW_ID }).catch(() => {});
</script>

<div class="titlebar">
  <div class="brand">
    <span class="mark">◆</span>
    {#if variant === "detached"}
      <span class="name detached-title">{title}</span>
    {:else}
      <span class="name">OBS MULTISTREAMER</span>
    {/if}
  </div>

  <div class="drag"></div>

  <div class="controls">
    {#if variant === "detached" && onRedock}
      <button class="redock" title="Return to main window" onclick={onRedock}>
        <Icon name="redock" size={12} />
        REDOCK
      </button>
    {/if}
    <button class="ctl" title="Minimize" aria-label="Minimize" onclick={minimize}>
      <Icon name="window-min" size={16} />
    </button>
    <button
      class="ctl"
      title={maximized ? "Restore" : "Maximize"}
      aria-label={maximized ? "Restore" : "Maximize"}
      onclick={toggleMax}
    >
      <Icon name={maximized ? "window-restore" : "window-max"} size={14} />
    </button>
    <button class="ctl close" title="Close" aria-label="Close" onclick={close}>
      <Icon name="window-close" size={16} />
    </button>
  </div>
</div>

<style>
  .titlebar {
    flex: 0 0 auto;
    display: flex;
    align-items: stretch;
    height: 32px;
    background: var(--color-rail);
    border-bottom: var(--border-weight) solid var(--color-border);
    /* The whole strip is the drag surface; interactive children opt out below.
       Chromium/CEF reads the -webkit- prefixed property. */
    -webkit-app-region: drag;
    user-select: none;
  }
  .brand {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 0 12px;
    min-width: 0;
  }
  .mark {
    color: var(--color-accent);
    font-size: 11px;
    line-height: 1;
  }
  .name {
    font-family: var(--font-mono);
    font-size: 10px;
    font-weight: 600;
    letter-spacing: 0.14em;
    text-transform: uppercase;
    color: var(--color-dim);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .detached-title {
    color: var(--color-accent);
    letter-spacing: 0.08em;
  }
  .drag {
    flex: 1 1 auto;
    min-width: 0;
  }
  .controls {
    display: flex;
    align-items: stretch;
    -webkit-app-region: no-drag;
  }
  .redock {
    display: flex;
    align-items: center;
    gap: 5px;
    height: auto;
    margin: 5px 6px;
    padding: 0 8px;
    font-family: var(--font-ui);
    font-size: 9px;
    letter-spacing: var(--letter-spacing);
    text-transform: uppercase;
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-muted);
  }
  .redock:hover {
    color: var(--color-accent);
    border-color: var(--color-accent);
  }
  .ctl {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 44px;
    height: 100%;
    padding: 0;
    background: transparent;
    border: 0;
    color: var(--color-muted);
  }
  .ctl:hover {
    background: color-mix(in srgb, var(--color-text) 10%, transparent);
    color: var(--color-text);
    border: 0;
  }
  .ctl.close:hover {
    background: var(--color-live);
    color: #fff;
  }
</style>
