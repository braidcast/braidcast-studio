<script lang="ts">
  import { onMount } from "svelte";
  import { obs, type SceneInfo } from "../bridge";
  import { defaultCanvas } from "./defaultCanvasStore.svelte";
  import ContextMenu, { type ContextMenuItem } from "../ContextMenu.svelte";
  import ListToolbar, { type ToolAction } from "../dock/ListToolbar.svelte";
  import FilterReveal from "../dock/FilterReveal.svelte";

  // The mount adapter strips internal __* keys; this dock declares no props.
  let {}: Record<string, unknown> = $props();

  onMount(() => {
    defaultCanvas.start();
    obs
      .call("settings.getGeneral")
      .then((g) => (gridMode = g.scenesGridMode))
      .catch(() => {});
    const offGeneral = obs.on("settings.generalChanged", (g) => (gridMode = g.scenesGridMode));
    return () => offGeneral();
  });

  // Session-only name filter; grid/list layout mirrors the persisted general setting.
  let filter = $state("");
  let gridMode = $state(false);

  const filteredScenes = $derived(
    filter.trim()
      ? defaultCanvas.scenes.filter((s) => s.name.toLowerCase().includes(filter.trim().toLowerCase()))
      : defaultCanvas.scenes,
  );

  function toggleGrid() {
    const next = !gridMode;
    gridMode = next; // optimistic; generalChanged reconciles
    obs.call("settings.setGeneral", { scenesGridMode: next }).catch(report);
  }

  // The bottom toolbar acts on the current (selected) scene, mirroring the
  // per-canvas CanvasDock chrome. Rename stays on dbl-click + the context menu.
  const currentName = $derived(defaultCanvas.scenes.find((s) => s.current)?.name ?? null);

  const leftActions = $derived<ToolAction[]>([
    { icon: "plus", title: "Add scene", onClick: beginAdd },
    {
      icon: "trash",
      title: "Remove scene",
      disabled: !currentName || defaultCanvas.scenes.length <= 1,
      onClick: () => currentName && void remove(currentName),
    },
  ]);
  const rightActions = $derived<ToolAction[]>([
    {
      icon: gridMode ? "list" : "grid",
      title: gridMode ? "List view" : "Grid view",
      active: gridMode,
      onClick: toggleGrid,
    },
  ]);

  let adding = $state(false);
  let newName = $state("");
  let renamingFrom = $state<string | null>(null);
  let renameTo = $state("");
  let actionError = $state<string | null>(null);
  let menu = $state<{ x: number; y: number; items: (ContextMenuItem | null)[] } | null>(null);

  function focusOnMount(node: HTMLInputElement) {
    node.focus();
    node.select();
  }

  function report(e: unknown) {
    actionError = (e as Error).message;
  }

  function setCurrent(name: string) {
    actionError = null;
    defaultCanvas.setCurrent(name).catch(report);
  }

  function beginAdd() {
    actionError = null;
    adding = true;
    newName = "";
  }

  async function commitAdd() {
    const name = newName.trim();
    adding = false;
    if (!name) {
      return;
    }
    actionError = null;
    try {
      await defaultCanvas.create(name);
    } catch (e) {
      report(e);
    }
  }

  function beginRename(name: string) {
    actionError = null;
    renamingFrom = name;
    renameTo = name;
  }

  async function commitRename() {
    const from = renamingFrom;
    const to = renameTo.trim();
    renamingFrom = null;
    if (!from || !to || to === from) {
      return;
    }
    actionError = null;
    try {
      await defaultCanvas.rename(from, to);
    } catch (e) {
      report(e);
    }
  }

  function duplicate(name: string) {
    actionError = null;
    defaultCanvas.duplicate(name).catch(report);
  }

  async function remove(name: string) {
    actionError = null;
    try {
      await defaultCanvas.remove(name);
    } catch (e) {
      report(e);
    }
  }

  function openMenu(e: MouseEvent, name: string) {
    e.preventDefault();
    menu = {
      x: e.clientX,
      y: e.clientY,
      items: [
        { label: "Rename", action: () => beginRename(name) },
        { label: "Duplicate", action: () => duplicate(name) },
        // Projector entries hidden pending the projector redesign.
        null,
        { label: "Remove", danger: true, disabled: defaultCanvas.scenes.length <= 1, action: () => void remove(name) },
      ],
    };
  }

  function onAddKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      void commitAdd();
    } else if (e.key === "Escape") {
      adding = false;
    }
  }

  function onRenameKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      void commitRename();
    } else if (e.key === "Escape") {
      renamingFrom = null;
    }
  }
</script>

<!-- Per-scene label/rename, shared by both layouts so selection, activation, and
     rename behave identically in list and grid. -->
{#snippet sceneCell(scene: SceneInfo)}
  {#if renamingFrom === scene.name}
    <input class="inline" bind:value={renameTo} onkeydown={onRenameKey} onblur={commitRename} use:focusOnMount />
  {:else}
    <button class="dock-label" ondblclick={() => beginRename(scene.name)} onclick={() => setCurrent(scene.name)}>
      {scene.name}
    </button>
  {/if}
{/snippet}

<div class="dock-body">
  <div class="dock-fill">
    {#if defaultCanvas.error}
      <p class="dock-msg err">{defaultCanvas.error}</p>
    {:else if !defaultCanvas.loaded}
      <p class="dock-msg">Loading…</p>
    {:else if gridMode}
    <div class="scene-grid">
      {#each filteredScenes as scene (scene.name)}
        <div class="grid-tile" class:sel={scene.current} oncontextmenu={(e) => openMenu(e, scene.name)} role="listitem">
          {@render sceneCell(scene)}
        </div>
      {/each}

      {#if adding}
        <div class="grid-tile">
          <input
            class="inline"
            placeholder="Scene name"
            bind:value={newName}
            onkeydown={onAddKey}
            onblur={commitAdd}
            use:focusOnMount
          />
        </div>
      {/if}
    </div>

    {#if filteredScenes.length === 0 && !adding}
      <p class="dock-msg">{filter.trim() ? "No matches" : "No scenes"}</p>
    {/if}
  {:else}
    <ul class="dock-list">
      {#each filteredScenes as scene (scene.name)}
        <li class="dock-row" class:sel={scene.current} oncontextmenu={(e) => openMenu(e, scene.name)}>
          {@render sceneCell(scene)}
        </li>
      {/each}

      {#if adding}
        <li class="dock-row">
          <input
            class="inline"
            placeholder="Scene name"
            bind:value={newName}
            onkeydown={onAddKey}
            onblur={commitAdd}
            use:focusOnMount
          />
        </li>
      {/if}
    </ul>

    {#if filteredScenes.length === 0 && !adding}
      <p class="dock-msg">{filter.trim() ? "No matches" : "No scenes"}</p>
    {/if}
  {/if}

  {#if actionError}
    <p class="dock-msg err">{actionError}</p>
  {/if}
  </div>

  <ListToolbar left={leftActions} right={rightActions}>
    {#snippet middle()}
      <FilterReveal bind:value={filter} />
    {/snippet}
  </ListToolbar>
</div>

{#if menu}
  <ContextMenu x={menu.x} y={menu.y} items={menu.items} onClose={() => (menu = null)} />
{/if}

<style>
  .inline {
    flex: 1;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-accent);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 3px 5px;
  }
  .inline:focus {
    outline: none;
  }
  /* Scroll region above the pinned bottom toolbar. */
  .dock-fill {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
    overflow: auto;
  }
  .scene-grid {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(96px, 1fr));
    gap: 4px;
    padding: 6px;
    align-content: start;
  }
  .grid-tile {
    display: flex;
    align-items: center;
    min-height: 44px;
    padding: 6px 8px;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    border-left: 3px solid transparent;
  }
  :root[data-selection-style="left-bar"] .grid-tile.sel {
    border-left-color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 12%, transparent);
  }
  :root[data-selection-style="fill"] .grid-tile.sel {
    background: color-mix(in srgb, var(--color-accent) 22%, transparent);
  }
  .grid-tile.sel :global(.dock-label) {
    color: var(--color-accent);
  }
</style>
