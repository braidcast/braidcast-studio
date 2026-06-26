<script lang="ts">
  import Menu, { type MenuItem } from "./Menu.svelte";
  import { DOCKS } from "./dock/dockRegistry";
  import { openSettings } from "./settingsOpener.svelte";
  import { openThemeEditor } from "./themeEditorOpener.svelte";
  import { defaultSelection, openTransform } from "./transformOpener.svelte";
  import { openAbout } from "./aboutOpener.svelte";
  import { onMount } from "svelte";
  import { obs, type Monitor, type CollectionInfo } from "./bridge";
  import CollectionDialog, { type DialogSpec } from "./CollectionDialog.svelte";

  // The Fullscreen Projector (Program) entries depend on the runtime monitor
  // list. The shared projectorMenu cache builds ContextMenu items; the menu bar
  // uses MenuItem, so enumerate here into local rune state and build the entries
  // inline (one per monitor). Loaded once on mount; the set rarely changes.
  let monitors = $state<Monitor[]>([]);
  // Scene collections: the live list (active flagged) + the prompt/confirm/alert
  // dialog the menu drives. Refreshed on mount and on every collections.changed
  // push (create/rename/remove/switch all emit it). The dropdown closes on a
  // document click, so New/Rename/Delete open a modal instead of an inline input.
  let collections = $state<CollectionInfo[]>([]);
  let dialog = $state<DialogSpec | null>(null);

  function refreshCollections() {
    obs
      .call("collections.list")
      .then((list) => (collections = list ?? []))
      .catch((e) => console.log("collections.list failed: " + (e as Error).message));
  }

  onMount(() => {
    obs
      .call("display.listMonitors")
      .then((res) => (monitors = res?.monitors ?? []))
      .catch((e) => console.log("display.listMonitors failed: " + (e as Error).message));
    refreshCollections();
    return obs.on("collections.changed", refreshCollections);
  });

  function showError(e: unknown) {
    dialog = { kind: "alert", title: "Scene Collections", message: (e as Error).message };
  }

  function switchCollection(id: string) {
    obs.call("collections.switch", { id }).catch(showError);
  }

  function newCollection() {
    dialog = {
      kind: "prompt",
      title: "New Scene Collection",
      confirmLabel: "Create",
      onCommit: (name) => {
        if (!name) {
          return;
        }
        obs.call("collections.create", { name }).catch(showError);
      },
    };
  }

  function renameCollection() {
    const active = collections.find((c) => c.active);
    if (!active) {
      return;
    }
    dialog = {
      kind: "prompt",
      title: "Rename Scene Collection",
      initial: active.name,
      confirmLabel: "Rename",
      onCommit: (name) => {
        if (!name || name === active.name) {
          return;
        }
        obs.call("collections.rename", { id: active.id, name }).catch(showError);
      },
    };
  }

  function deleteCollection() {
    const active = collections.find((c) => c.active);
    if (!active) {
      return;
    }
    dialog = {
      kind: "confirm",
      title: "Delete Scene Collection",
      message: `Delete "${active.name}"? This cannot be undone.`,
      confirmLabel: "Delete",
      onCommit: () => {
        obs.call("collections.remove", { id: active.id }).catch(showError);
      },
    };
  }
  function openProgramFullscreen(monitor: number) {
    obs
      .call("projector.open", { target: { kind: "program" }, mode: "fullscreen", monitor })
      .catch((e) => console.log("projector.open failed: " + (e as Error).message));
  }
  function openProgramWindowed() {
    obs
      .call("projector.open", { target: { kind: "program" }, mode: "windowed" })
      .catch((e) => console.log("projector.open failed: " + (e as Error).message));
  }

  // App passes the dock-visibility map + the toggle / reset / lock actions so the
  // Docks menu drives the live layout. visibleDocks[id] === false => hidden.
  let {
    visibleDocks,
    toggleDock,
    resetLayout,
    locked,
    toggleLock,
  }: {
    visibleDocks: Record<string, boolean>;
    toggleDock: (id: string) => void;
    resetLayout: () => void;
    locked: boolean;
    toggleLock: () => void;
  } = $props();

  // §3.5 LOCKED contents. Actions not in P1 scope are present but disabled so the
  // menu shape matches the spec exactly; later phases enable them.
  const fileItems: (MenuItem | null)[] = $derived([
    { label: "Settings", action: () => openSettings("canvases") },
    { label: "Recordings", disabled: true },
    null,
    { label: "Exit", action: () => window.close() },
  ]);
  // Transform enables for the current default-canvas (channel-0) selection, tracked
  // by transformOpener via the global sceneItem.selected push.
  const editItems: (MenuItem | null)[] = $derived([
    { label: "Undo", disabled: true },
    { label: "Redo", disabled: true },
    null,
    {
      label: "Transform",
      disabled: defaultSelection.id == null,
      action: () =>
        defaultSelection.id != null &&
        openTransform({ scene: defaultSelection.scene ?? undefined, id: defaultSelection.id }, "Selected Source"),
    },
  ]);
  const viewItems: (MenuItem | null)[] = $derived([
    { label: "Studio Mode", disabled: true },
    // Toggles the on-demand Stats dock through the same add/remove path the Docks
    // menu uses (checked = currently present in the layout).
    { label: "Stats", checked: visibleDocks["stats"] !== false, action: () => toggleDock("stats") },
    { label: "Fullscreen Preview", disabled: true },
    null,
    { label: "Windowed Projector (Program)", action: openProgramWindowed },
    ...monitors.map((m) => ({
      label: `Fullscreen Projector (Program) — ${m.name} (${m.width}×${m.height})`,
      action: () => openProgramFullscreen(m.index),
    })),
  ]);
  // Docks menu: one toggle per dock (checked = visible) + Reset + Lock + the theme
  // editor (preset switching + full token editing live in the editor now).
  const dockItems: (MenuItem | null)[] = $derived([
    ...DOCKS.map((d) => ({
      label: d.title,
      checked: visibleDocks[d.id] !== false,
      action: () => toggleDock(d.id),
    })),
    null,
    { label: "Reset Layout", action: resetLayout },
    { label: "Lock Docks", checked: locked, action: toggleLock },
    null,
    { label: "Theme Editor…", action: () => openThemeEditor() },
  ]);
  // Scene Collection menu: the collections (active radio-checked → switch on click),
  // a divider, then New / Rename / Delete (Delete disabled with only one left).
  const collectionItems: (MenuItem | null)[] = $derived([
    ...collections.map((c) => ({
      label: c.name,
      checked: c.active,
      action: () => {
        if (!c.active) {
          switchCollection(c.id);
        }
      },
    })),
    null,
    { label: "New…", action: newCollection },
    { label: "Rename…", action: renameCollection },
    { label: "Delete", disabled: collections.length <= 1, action: deleteCollection },
  ]);
  const helpItems: (MenuItem | null)[] = [{ label: "About OBS MultiStream", action: () => openAbout() }];
</script>

<div class="menubar">
  <Menu label="File" items={fileItems} />
  <Menu label="Edit" items={editItems} />
  <Menu label="View" items={viewItems} />
  <Menu label="Docks ▾" items={dockItems} />
  <Menu label="Scene Collection ▾" items={collectionItems} />
  <Menu label="Help" items={helpItems} />
</div>

{#if dialog}
  <CollectionDialog {...dialog} onClose={() => (dialog = null)} />
{/if}

<style>
  .menubar {
    display: flex;
    gap: 4px;
    align-items: center;
    background: var(--color-surface);
    border-bottom: var(--border-weight) solid var(--color-border);
    padding: 2px 6px;
    flex: 0 0 auto;
  }
</style>
