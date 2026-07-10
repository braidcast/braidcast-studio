<script lang="ts">
  import { onMount, onDestroy } from "svelte";
  import { obs, type SceneItem, type ReorderDirection } from "../bridge";
import { EV } from "../eventNames";
  import { defaultCanvas } from "./defaultCanvasStore.svelte";
  import AddSourceModal from "../AddSourceModal.svelte";
  import PropertyForm from "../properties/PropertyForm.svelte";
  import Modal from "../Modal.svelte";
  import { suspendPreview } from "../previewGate.svelte";
  import ContextMenu, { type ContextMenuItem } from "../ContextMenu.svelte";
  import { clipboard } from "../clipboardStore.svelte";
  import { sourceSelection } from "../sourceSelectionStore.svelte";
  import { openFilters } from "../filterDialogOpener.svelte";
  import { transformMenu } from "../transformMenu";
  import { scaleFilterMenu } from "../scaleFilterMenu";
  import { deinterlaceMenu } from "../deinterlaceMenu";
  import { colorMenu } from "../colorMenu";
  import type { DeinterlaceMode, DeinterlaceFieldOrder } from "../bridge";
  import Icon from "../dock/Icon.svelte";
  import ListToolbar, { type ToolAction } from "../dock/ListToolbar.svelte";
  import FilterReveal from "../dock/FilterReveal.svelte";

  let {}: Record<string, unknown> = $props();

  onMount(() => {
    defaultCanvas.start();
  });

  // The default canvas's current scene drives this list (global channel-0 path).
  const currentScene = $derived(defaultCanvas.current);

  let items = $state<SceneItem[]>([]);
  // Session-only name filter. Reorder is disabled while filtering since the
  // up/down indices only make sense against the full, unfiltered ordering.
  let filter = $state("");
  const filtering = $derived(filter.trim().length > 0);
  const filteredItems = $derived(
    filtering ? items.filter((i) => (i.source ?? "").toLowerCase().includes(filter.trim().toLowerCase())) : items,
  );
  let loaded = $state(false);
  let error = $state<string | null>(null);
  let selectedItemId = $state<number | null>(null);
  let propsForSource = $state<string | null>(null);
  let adding = $state(false);
  let renamingId = $state<number | null>(null);
  let renameTo = $state("");
  let menu = $state<{ x: number; y: number; items: (ContextMenuItem | null)[] } | null>(null);

  // The bottom toolbar acts on the selected row (OBS list convention). Reorder is
  // indexed against the full, unfiltered `items` so up/down disable at the ends.
  const selectedItem = $derived(items.find((i) => i.id === selectedItemId) ?? null);
  const selectedIdx = $derived(selectedItemId === null ? -1 : items.findIndex((i) => i.id === selectedItemId));

  const leftActions = $derived<ToolAction[]>([
    { icon: "plus", title: "Add source", disabled: !currentScene, onClick: () => (adding = true) },
    { icon: "trash", title: "Remove source", disabled: !selectedItem, onClick: () => selectedItem && void remove(selectedItem) },
    {
      icon: "gear",
      title: "Properties",
      disabled: !selectedItem?.source,
      onClick: () => selectedItem && openProperties(selectedItem),
    },
  ]);
  const rightActions = $derived<ToolAction[]>([
    {
      icon: "up",
      title: "Move up",
      disabled: filtering || selectedIdx <= 0,
      onClick: () => selectedItem && void reorder(selectedItem, "up"),
    },
    {
      icon: "down",
      title: "Move down",
      disabled: filtering || selectedIdx < 0 || selectedIdx >= items.length - 1,
      onClick: () => selectedItem && void reorder(selectedItem, "down"),
    },
  ]);

  function report(e: unknown) {
    error = (e as Error).message;
  }

  function focusOnMount(node: HTMLInputElement) {
    node.focus();
    node.select();
  }

  function openProperties(item: SceneItem) {
    if (item.source) {
      propsForSource = item.source;
    }
  }

  function selectItem(item: SceneItem) {
    selectedItemId = item.id;
    void obs.call("preview.select", { scene: currentScene, id: item.id }).catch(() => {});
  }

  // The properties modal overlaps the preview; suspend the native overlay while open.
  $effect(() => {
    if (propsForSource) {
      return suspendPreview();
    }
  });

  // Reflect preview-driven selection (click in the overlay) back into the list.
  $effect(() => {
    return obs.on(EV.sceneItemSelected, (p) => {
      // Global channel-0 path: only the Default surface (canvas=null) drives this.
      if (p.canvas == null && (!p.scene || p.scene === currentScene)) {
        selectedItemId = p.id;
      }
    });
  });

  // Request-generation guard: a fast scene switch must not let a slow prior list
  // response overwrite the newer scene's sources.
  let loadSeq = 0;
  async function load() {
    if (!currentScene) {
      items = [];
      loaded = true;
      return;
    }
    const mine = ++loadSeq;
    error = null;
    try {
      const list = await obs.call("sceneItems.list", { scene: currentScene });
      if (mine !== loadSeq) {
        return;
      }
      items = list;
      if (selectedItemId !== null && !items.some((i) => i.id === selectedItemId)) {
        selectedItemId = null;
      }
    } catch (e) {
      report(e);
    } finally {
      loaded = true;
    }
  }

  // Reload whenever the current scene changes.
  $effect(() => {
    void currentScene;
    void load();
  });

  // Publish the global selection so the app-level Ctrl+C/Ctrl+V can act on it.
  $effect(() => {
    sourceSelection.scene = currentScene;
    sourceSelection.item = items.find((i) => i.id === selectedItemId) ?? null;
  });

  // Drop our published selection on teardown so the app-level Ctrl+C/Ctrl+V doesn't
  // act on a stale scene/item after this dock unmounts — but only if the store still
  // points at what we published (another surface may have taken over).
  onDestroy(() => {
    if (sourceSelection.scene === currentScene) {
      sourceSelection.scene = null;
      sourceSelection.item = null;
    }
  });

  // Refresh on item mutations targeting the global path (canvas=null) for our scene.
  $effect(() => {
    return obs.on(EV.sceneItemsChanged, (p) => {
      if (p.canvas == null && (!p.scene || p.scene === currentScene)) {
        void load();
      }
    });
  });

  function onSourceCreated(created: { id: number; source: string }) {
    adding = false;
    selectedItemId = created.id;
    propsForSource = created.source;
    void load();
  }

  async function toggleVisible(item: SceneItem) {
    try {
      await obs.call("sceneItems.setVisible", { scene: currentScene, id: item.id, visible: !item.visible });
    } catch (e) {
      report(e);
    }
  }

  async function toggleLocked(item: SceneItem) {
    try {
      await obs.call("sceneItems.setLocked", { scene: currentScene, id: item.id, locked: !item.locked });
    } catch (e) {
      report(e);
    }
  }

  async function reorder(item: SceneItem, direction: ReorderDirection) {
    try {
      await obs.call("sceneItems.reorder", { scene: currentScene, id: item.id, direction });
    } catch (e) {
      report(e);
    }
  }

  async function remove(item: SceneItem) {
    try {
      await obs.call("sceneItems.remove", { scene: currentScene, id: item.id });
    } catch (e) {
      report(e);
    }
  }

  function beginRename(item: SceneItem) {
    renamingId = item.id;
    renameTo = item.source ?? "";
  }

  async function commitRename() {
    const id = renamingId;
    const name = renameTo.trim();
    renamingId = null;
    if (id === null || !name) {
      return;
    }
    try {
      await obs.call("sources.rename", { scene: currentScene, id, name });
    } catch (e) {
      report(e);
    }
  }

  function onRenameKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      void commitRename();
    } else if (e.key === "Escape") {
      renamingId = null;
    }
  }

  // ---- clipboard actions (copy/paste/duplicate/filters/transform/group) ------
  // All target the global channel-0 path (no canvas); paste lands in currentScene.
  function copySource(item: SceneItem) {
    if (item.source) {
      clipboard.source = { ref: item.source, name: item.source };
    }
  }

  async function pasteSource() {
    if (!clipboard.source || !currentScene) {
      return;
    }
    try {
      await obs.call("sources.addExisting", { scene: currentScene, name: clipboard.source.ref });
    } catch (e) {
      report(e);
    }
  }

  async function duplicateItem(item: SceneItem) {
    try {
      await obs.call("sources.duplicate", { scene: currentScene, id: item.id });
    } catch (e) {
      report(e);
    }
  }

  async function copyFilters(item: SceneItem) {
    if (!item.source) {
      return;
    }
    try {
      clipboard.filters = (await obs.call("filters.copyChain", { source: item.source })).filters;
    } catch (e) {
      report(e);
    }
  }

  async function pasteFilters(item: SceneItem) {
    if (!item.source || !clipboard.filters) {
      return;
    }
    try {
      await obs.call("filters.pasteChain", { source: item.source, filters: clipboard.filters });
    } catch (e) {
      report(e);
    }
  }

  async function copyTransform(item: SceneItem) {
    try {
      clipboard.transform = await obs.call("sceneItems.getTransform", { scene: currentScene, id: item.id });
    } catch (e) {
      report(e);
    }
  }

  async function pasteTransform(item: SceneItem) {
    if (!clipboard.transform) {
      return;
    }
    try {
      await obs.call("sceneItems.setTransform", { scene: currentScene, id: item.id, transform: clipboard.transform });
    } catch (e) {
      report(e);
    }
  }

  async function groupItem(item: SceneItem) {
    try {
      await obs.call("sceneItems.group", { scene: currentScene, ids: [item.id] });
    } catch (e) {
      report(e);
    }
  }

  async function ungroupItem(item: SceneItem) {
    try {
      await obs.call("sceneItems.ungroup", { scene: currentScene, id: item.id });
    } catch (e) {
      report(e);
    }
  }

  // Deinterlacing lives on the source (not the scene item), so it isn't in the row
  // data; fetch it just-in-time when the menu opens. Falls back to disabled state.
  async function fetchDeint(source: string): Promise<{ mode: DeinterlaceMode; fieldOrder: DeinterlaceFieldOrder }> {
    try {
      return await obs.call("sources.getDeinterlace", { source });
    } catch {
      return { mode: "disable", fieldOrder: "top" };
    }
  }

  async function openMenu(e: MouseEvent, item: SceneItem, idx: number) {
    e.preventDefault();
    const x = e.clientX;
    const y = e.clientY;
    const deint = item.source ? await fetchDeint(item.source) : { mode: "disable" as const, fieldOrder: "top" as const };
    menu = {
      x,
      y,
      items: [
        { label: "Properties", action: () => openProperties(item) },
        { label: "Filters", disabled: !item.source, action: () => item.source && openFilters(item.source) },
        ...(item.interactive && item.source
          ? [{ label: "Interact", action: () => void obs.call("sources.interact", { source: item.source }).catch(report) }]
          : []),
        transformMenu({ scene: currentScene ?? undefined, id: item.id }, item.source ?? "(unnamed)"),
        { label: "Rename", action: () => beginRename(item) },
        scaleFilterMenu(item.scaleFilter, (filter) =>
          void obs.call("sceneItems.setScaleFilter", { scene: currentScene, id: item.id, filter }).catch(report),
        ),
        ...(item.source
          ? [
              deinterlaceMenu(
                deint.mode,
                deint.fieldOrder,
                (mode) => void obs.call("sources.setDeinterlace", { source: item.source, mode }).catch(report),
                (fieldOrder) =>
                  void obs.call("sources.setDeinterlace", { source: item.source, fieldOrder }).catch(report),
              ),
            ]
          : []),
        colorMenu(item.color, (color) =>
          void obs.call("sceneItems.setColor", { scene: currentScene, id: item.id, color }).catch(report),
        ),
        {
          label: "Screenshot",
          disabled: !item.source,
          action: () => void obs.call("screenshot.takeSource", { scene: currentScene, id: item.id }).catch(report),
        },
        null,
        { label: "Copy", disabled: !item.source, action: () => copySource(item) },
        { label: "Paste", disabled: !clipboard.source, action: () => void pasteSource() },
        { label: "Duplicate", action: () => void duplicateItem(item) },
        null,
        { label: "Copy Filters", disabled: !item.source, action: () => void copyFilters(item) },
        { label: "Paste Filters", disabled: !clipboard.filters, action: () => void pasteFilters(item) },
        { label: "Copy Transform", action: () => void copyTransform(item) },
        { label: "Paste Transform", disabled: !clipboard.transform, action: () => void pasteTransform(item) },
        null,
        { label: "Group", action: () => void groupItem(item) },
        { label: "Ungroup", action: () => void ungroupItem(item) },
        null,
        { label: item.visible ? "Hide" : "Show", action: () => void toggleVisible(item) },
        { label: item.locked ? "Unlock" : "Lock", action: () => void toggleLocked(item) },
        null,
        { label: "Move Up", disabled: filtering || idx === 0, action: () => void reorder(item, "up") },
        { label: "Move Down", disabled: filtering || idx === items.length - 1, action: () => void reorder(item, "down") },
        { label: "Move to Top", disabled: filtering || idx === 0, action: () => void reorder(item, "top") },
        {
          label: "Move to Bottom",
          disabled: filtering || idx === items.length - 1,
          action: () => void reorder(item, "bottom"),
        },
        // Projector entries hidden pending the projector redesign (projectorMenu +
        // its bridge path are kept, just not surfaced here).
        null,
        { label: "Remove", danger: true, action: () => void remove(item) },
      ],
    };
  }
</script>

<div class="dock-body">
  <div class="dock-fill">
    {#if error}
      <p class="dock-msg err">{error}</p>
    {:else if !currentScene}
      <p class="dock-msg">No scene selected</p>
    {:else if !loaded}
      <p class="dock-msg">Loading…</p>
    {:else if items.length === 0}
      <p class="dock-msg">No sources</p>
    {:else if filteredItems.length === 0}
      <p class="dock-msg">No matches</p>
    {:else}
      <ul class="dock-list">
      {#each filteredItems as item, idx (item.id)}
        <li
          class="dock-row"
          class:sel={item.id === selectedItemId}
          class:dimmed={!item.visible}
          style:box-shadow={item.color ? `inset 3px 0 0 ${item.color}` : null}
          oncontextmenu={(e) => void openMenu(e, item, idx)}
        >
          <button
            class="dock-toggle"
            class:off={!item.visible}
            title={item.visible ? "Hide" : "Show"}
            onclick={() => void toggleVisible(item)}><Icon name={item.visible ? "eye" : "eye-off"} size={14} /></button
          >
          <button
            class="dock-toggle"
            title={item.locked ? "Unlock" : "Lock"}
            onclick={() => void toggleLocked(item)}><Icon name={item.locked ? "lock" : "lock-open"} size={12} /></button
          >
          {#if renamingId === item.id}
            <input class="inline" bind:value={renameTo} onkeydown={onRenameKey} onblur={commitRename} use:focusOnMount />
          {:else}
            <button class="dock-label" onclick={() => selectItem(item)} ondblclick={() => openProperties(item)}>
              {item.source ?? "(unnamed)"}
            </button>
          {/if}
        </li>
      {/each}
      </ul>
    {/if}
  </div>

  <ListToolbar left={leftActions} right={rightActions}>
    {#snippet middle()}
      <FilterReveal bind:value={filter} />
    {/snippet}
  </ListToolbar>
</div>

{#if adding}
  <AddSourceModal canvas={null} scene={currentScene} onCreated={onSourceCreated} onClose={() => (adding = false)} />
{/if}

{#if propsForSource}
  <Modal title={"Properties — " + propsForSource} onClose={() => (propsForSource = null)} width={560} maxHeight="80vh">
    <PropertyForm kind="source" ref={propsForSource} />
  </Modal>
{/if}

{#if menu}
  <ContextMenu x={menu.x} y={menu.y} items={menu.items} onClose={() => (menu = null)} />
{/if}

<style>
  /* Sources rows are one row shorter than the shared default (5px 7px). */
  .dock-row {
    padding: 4px 7px;
  }
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
  /* Scroll region above the pinned bottom toolbar, so the toolbar stays at the
     dock's foot even when the list is empty or short. */
  .dock-fill {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
    overflow: auto;
  }
</style>
