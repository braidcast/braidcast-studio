<script lang="ts">
  import { onMount, onDestroy } from "svelte";
  import { createDockview, type DockviewApi, type DockviewComponentOptions, type IContentRenderer } from "dockview-core";
  import "dockview-core/dist/styles/dockview.css";
  import type { Component } from "svelte";
  import { DOCKS } from "./dockRegistry";
  import { createContentRenderer } from "./mountAdapter";
  import { DockTab } from "./tabRenderer";
  import CanvasDock from "../docks/CanvasDock.svelte";

  // onReady hands the caller the DockviewApi (fired from this host's onMount) so it
  // can build the default layout + save/load. The caller adds panels with the
  // standalone panelOptions(id, detachDock, ...) from dockRegistry — a free function
  // rather than a method here, so layout building does not depend on a `bind:this`
  // ref Svelte 5 only assigns AFTER this child mounts (onReady fires during it).
  let { onReady }: { onReady?: (api: DockviewApi) => void } = $props();

  let container: HTMLDivElement;
  let api: DockviewApi | undefined;

  // Map each dock id to its Svelte content renderer factory once. Dockview resolves
  // a panel's `component` string (== dock id) to one of these.
  const renderers: Record<string, () => IContentRenderer> = {};
  for (const d of DOCKS) {
    renderers[d.id] = createContentRenderer(d.component);
  }
  // Dynamic per-canvas composite docks (canvasReconciler adds them at runtime with
  // component "canvas" + JSON-safe params {canvasUuid, canvasName}); register the
  // one shared renderer here since they are not static DOCKS entries.
  renderers["canvas"] = createContentRenderer(CanvasDock as unknown as Component<Record<string, unknown>>);

  function buildOptions(): DockviewComponentOptions {
    return {
      // Content: the mounted Svelte body, keyed by the panel's component (dock id).
      createComponent: (options) => renderers[options.name](),
      // Header chrome (title + ⧉ + ✕): a single custom tab renderer for every dock.
      // defaultTabComponent names it so panels need not set tabComponent per-panel.
      defaultTabComponent: "obs-tab",
      createTabComponent: () => new DockTab(),
    };
  }

  export function getApi(): DockviewApi | undefined {
    return api;
  }

  onMount(() => {
    api = createDockview(container, buildOptions());
    onReady?.(api);
  });

  onDestroy(() => {
    api?.dispose();
    api = undefined;
  });
</script>

<div class="dockview-theme-obs dock-host" bind:this={container}></div>

<style>
  .dock-host {
    height: 100%;
    width: 100%;
    min-height: 0;
  }
</style>
