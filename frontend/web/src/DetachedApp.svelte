<script lang="ts">
  import { onMount, mount, unmount, type Component } from "svelte";
  import { obs } from "./lib/bridge";
  import { themeStore } from "./lib/theme/themeStore.svelte";
  import { WINDOW_ID, DETACHED_DOCK } from "./lib/windowContext";
  import { dockById } from "./lib/dock/dockRegistry";
  import CanvasDock from "./lib/docks/CanvasDock.svelte";
  import BrowserDock from "./lib/docks/BrowserDock.svelte";
  import { browserDockStore } from "./lib/browserDockStore.svelte";
  import type { CanvasInfo } from "./lib/bridge";
  import Icon from "./lib/dock/Icon.svelte";

  // Apply the saved (or default Industrial) theme in this window too.
  void themeStore.hydrate();

  let host = $state<HTMLDivElement | undefined>();
  let title = $state(DETACHED_DOCK ?? "");
  let mounted: Record<string, unknown> | null = null;

  async function resolveAndMount(): Promise<void> {
    if (!host || !DETACHED_DOCK) return;
    let comp: Component<Record<string, unknown>> | undefined;
    let props: Record<string, unknown> = {};
    if (DETACHED_DOCK.startsWith("canvas:")) {
      const uuid = DETACHED_DOCK.slice("canvas:".length);
      let name = "Canvas";
      try {
        const list = await obs.call("canvas.list");
        name = list.find((c: CanvasInfo) => c.uuid === uuid)?.name ?? "Canvas";
      } catch {
        // fall back to a generic name
      }
      comp = CanvasDock as unknown as Component<Record<string, unknown>>;
      props = { canvasUuid: uuid, canvasName: name };
      title = "Canvas · " + name;
    } else if (DETACHED_DOCK.startsWith("browserdock:")) {
      // A user-defined browser dock: resolve its {url,title} from the store by id
      // (loaded via the store's own bridge call in this window) and render the same
      // iframe body the main-window panel uses.
      const id = DETACHED_DOCK.slice("browserdock:".length);
      await browserDockStore.load();
      const dock = browserDockStore.byId(id);
      if (!dock) return;
      comp = BrowserDock as unknown as Component<Record<string, unknown>>;
      props = { url: dock.url, title: dock.title };
      title = dock.title || "Browser";
    } else {
      const def = dockById(DETACHED_DOCK);
      if (!def) return;
      comp = def.component;
      props = { ...def.params };
      title = def.title;
    }
    mounted = mount(comp, { target: host, props });
  }

  function redock(): void {
    void obs.call("window.redock", { windowId: WINDOW_ID }).catch(() => {});
  }

  onMount(() => {
    void resolveAndMount();
    return () => {
      if (mounted) void unmount(mounted);
    };
  });
</script>

<div class="detached">
  <header class="bar">
    <span class="title">{title}</span>
    <button class="redock" title="Return to main window" onclick={redock}>
      <Icon name="redock" size={12} />
      REDOCK
    </button>
  </header>
  <div class="body" bind:this={host}></div>
</div>

<style>
  .detached {
    display: flex;
    flex-direction: column;
    height: 100%;
    background: var(--color-base);
  }
  .bar {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 4px 8px;
    background: var(--color-surface);
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .title {
    font-family: var(--font-ui);
    font-size: 9px;
    letter-spacing: var(--letter-spacing);
    text-transform: uppercase;
    color: var(--color-accent);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .redock {
    height: auto;
    display: flex;
    align-items: center;
    gap: 5px;
    padding: 2px 8px;
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
  .body {
    flex: 1;
    min-height: 0;
    overflow: auto;
  }
</style>
