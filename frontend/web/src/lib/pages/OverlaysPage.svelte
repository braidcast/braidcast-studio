<script lang="ts">
  // Overlays page (master-detail): a left list of overlay widgets + a right tabbed
  // editor (Code / Fields / Preview). Widgets are loopback-SSE overlays served by the
  // C++ Overlay::Server; the user copies a widget URL into an OBS Browser Source. Edits
  // mutate a local $state copy and debounce into overlays.update (~500ms), then bump
  // reloadKey so the preview iframe reloads with the freshly-assembled document. A Save
  // button flushes immediately. Create/Duplicate/Delete + the host's overlays.changed
  // push keep the list in sync.
  import { onMount, onDestroy } from "svelte";
  import { obs, type OverlayListItem, type OverlayWidget, type OverlayField } from "../bridge";
  import CodePane from "../overlays/CodePane.svelte";
  import FieldsDesigner from "../overlays/FieldsDesigner.svelte";
  import PreviewPane from "../overlays/PreviewPane.svelte";
  import CollectionDialog, { type DialogSpec } from "../CollectionDialog.svelte";

  let items = $state<OverlayListItem[]>([]);
  let selectedId = $state<string | null>(null);
  let widget = $state<OverlayWidget | null>(null);
  let tab = $state<"code" | "fields" | "preview">("code");
  let reloadKey = $state(0);
  let portChanged = $state(false);
  let loaded = $state(false);
  let error = $state<string | null>(null);
  let copiedId = $state<string | null>(null);
  let saving = $state(false);
  let dialog = $state<DialogSpec | null>(null);

  let saveTimer: ReturnType<typeof setTimeout> | undefined;
  let copiedTimer: ReturnType<typeof setTimeout> | undefined;

  function refresh(): void {
    obs
      .call("overlays.list")
      .then((l) => {
        items = l ?? [];
        // Drop a selection whose widget vanished (deleted elsewhere).
        if (selectedId && !items.some((i) => i.id === selectedId)) {
          selectedId = null;
          widget = null;
        }
      })
      .catch((e) => (error = (e as Error).message))
      .finally(() => (loaded = true));
  }

  async function select(id: string): Promise<void> {
    if (id === selectedId) {
      return;
    }
    // Flush any pending edit to the outgoing widget before switching.
    await flushSave();
    selectedId = id;
    tab = "code";
    try {
      widget = await obs.call("overlays.get", { id });
      reloadKey++;
    } catch (e) {
      error = (e as Error).message;
      widget = null;
    }
  }

  function scheduleSave(): void {
    if (!widget) {
      return;
    }
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => void flushSave(), 500);
  }

  async function flushSave(): Promise<void> {
    clearTimeout(saveTimer);
    if (!widget) {
      return;
    }
    const w = widget;
    saving = true;
    try {
      await obs.call("overlays.update", {
        id: w.id,
        name: w.name,
        html: w.html,
        css: w.css,
        js: w.js,
        fields: w.fields,
      });
      reloadKey++;
      // Keep the list row's name label in sync without a full re-fetch.
      items = items.map((it) => (it.id === w.id ? { ...it, name: w.name } : it));
    } catch (e) {
      error = (e as Error).message;
    } finally {
      saving = false;
    }
  }

  async function create(): Promise<void> {
    try {
      const w = await obs.call("overlays.create", { name: "New Overlay", type: "alertbox" });
      refresh();
      await select(w.id);
    } catch (e) {
      error = (e as Error).message;
    }
  }

  async function duplicate(): Promise<void> {
    if (!selectedId) {
      return;
    }
    try {
      const w = await obs.call("overlays.duplicate", { id: selectedId });
      refresh();
      await select(w.id);
    } catch (e) {
      error = (e as Error).message;
    }
  }

  function confirmDelete(): void {
    const target = widget;
    if (!target) {
      return;
    }
    dialog = {
      kind: "confirm",
      title: "Delete Overlay",
      message: `Delete "${target.name}"? This removes the widget and its uploaded assets.`,
      confirmLabel: "Delete",
      onCommit: () => void remove(target.id),
    };
  }

  async function remove(id: string): Promise<void> {
    try {
      await obs.call("overlays.delete", { id });
      if (selectedId === id) {
        selectedId = null;
        widget = null;
      }
      refresh();
    } catch (e) {
      error = (e as Error).message;
    }
  }

  async function copyUrl(item: OverlayListItem): Promise<void> {
    try {
      await navigator.clipboard.writeText(item.url);
      copiedId = item.id;
      clearTimeout(copiedTimer);
      copiedTimer = setTimeout(() => (copiedId = null), 1400);
    } catch (e) {
      error = "Copy failed: " + (e as Error).message;
    }
  }

  async function addToScene(item: OverlayListItem): Promise<void> {
    try {
      await obs.call("overlays.addToScene", { id: item.id });
    } catch (e) {
      error = (e as Error).message;
    }
  }

  // --- editor field bindings (mutate local widget, then debounce the update) ---
  function onHtml(v: string): void {
    if (widget) {
      widget.html = v;
      scheduleSave();
    }
  }
  function onCss(v: string): void {
    if (widget) {
      widget.css = v;
      scheduleSave();
    }
  }
  function onJs(v: string): void {
    if (widget) {
      widget.js = v;
      scheduleSave();
    }
  }
  function onFields(f: OverlayField[]): void {
    if (widget) {
      widget.fields = f;
      scheduleSave();
    }
  }
  function onName(v: string): void {
    if (widget) {
      widget.name = v;
      scheduleSave();
    }
  }

  onMount(() => {
    refresh();
    obs
      .call("overlays.serverInfo")
      .then((s) => (portChanged = !!s?.portChanged))
      .catch(() => {});
    return obs.on("overlays.changed", refresh);
  });

  onDestroy(() => {
    clearTimeout(saveTimer);
    clearTimeout(copiedTimer);
  });
</script>

<div class="page">
  <header class="head">
    <span class="title">Overlays</span>
    <span class="sub">loopback widgets · copy a URL into a browser source</span>
  </header>

  {#if portChanged}
    <div class="banner">
      Overlay port changed since last run — re-copy your widget URLs into their Browser Sources.
    </div>
  {/if}

  <div class="body">
    <nav class="subnav" aria-label="Overlays">
      {#if loaded}
        {#each items as it (it.id)}
          <div class="nav-item" class:active={it.id === selectedId}>
            <button
              class="nav-main"
              aria-current={it.id === selectedId ? "page" : undefined}
              onclick={() => void select(it.id)}
            >
              <span class="nav-name">{it.name}</span>
              <span class="nav-badge">{it.type}</span>
            </button>
            <div class="nav-actions">
              <button
                class="mini-btn"
                class:copied={copiedId === it.id}
                title="Copy widget URL"
                onclick={() => void copyUrl(it)}
              >
                {copiedId === it.id ? "Copied" : "Copy URL"}
              </button>
              <button class="mini-btn" title="Add a Browser Source to the current scene" onclick={() => void addToScene(it)}>
                Add to scene
              </button>
            </div>
          </div>
        {/each}
      {/if}
      <button class="addnav" onclick={() => void create()}>＋ New overlay</button>
    </nav>

    <div class="pane">
      {#if error}<p class="err">{error}</p>{/if}

      {#if !loaded}
        <p class="dim">Loading overlays…</p>
      {:else if !widget}
        <p class="dim">No overlay selected. Create one, or pick a widget from the list.</p>
      {:else}
        <div class="editor">
          <div class="editor-bar">
            <input
              class="name-in"
              value={widget.name}
              aria-label="Overlay name"
              oninput={(e) => onName(e.currentTarget.value)}
            />
            <div class="tabs">
              <button class="tab" class:on={tab === "code"} onclick={() => (tab = "code")}>Code</button>
              <button class="tab" class:on={tab === "fields"} onclick={() => (tab = "fields")}>Fields</button>
              <button class="tab" class:on={tab === "preview"} onclick={() => (tab = "preview")}>Preview</button>
            </div>
            <span class="editor-spacer"></span>
            <span class="save-state">{saving ? "Saving…" : "Saved"}</span>
            <button class="accent" onclick={() => void flushSave()}>Save</button>
            <button class="ghost" onclick={duplicate}>Duplicate</button>
            <button class="ghost danger" onclick={confirmDelete}>Delete</button>
          </div>

          <div class="editor-body">
            {#if tab === "code"}
              <div class="code-grid">
                <div class="code-cell">
                  <span class="cell-kicker">HTML</span>
                  <CodePane value={widget.html} lang="html" onChange={onHtml} />
                </div>
                <div class="code-cell">
                  <span class="cell-kicker">CSS</span>
                  <CodePane value={widget.css} lang="css" onChange={onCss} />
                </div>
                <div class="code-cell">
                  <span class="cell-kicker">JS</span>
                  <CodePane value={widget.js} lang="javascript" onChange={onJs} />
                </div>
              </div>
            {:else if tab === "fields"}
              <div class="scroll-pane">
                <FieldsDesigner fields={widget.fields} widgetId={widget.id} onChange={onFields} />
              </div>
            {:else}
              <PreviewPane url={widget.url} widgetId={widget.id} {reloadKey} />
            {/if}
          </div>
        </div>
      {/if}
    </div>
  </div>
</div>

{#if dialog}
  <CollectionDialog {...dialog} onClose={() => (dialog = null)} />
{/if}

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
    background: var(--color-base);
    color: var(--color-text);
  }
  .head {
    flex: 0 0 auto;
    height: 58px;
    display: flex;
    align-items: baseline;
    gap: 12px;
    padding: 0 24px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .title {
    font-family: var(--font-ui);
    font-size: 16px;
    font-weight: 600;
    letter-spacing: -0.01em;
  }
  .sub {
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
  }
  .banner {
    flex: 0 0 auto;
    padding: 8px 24px;
    background: color-mix(in srgb, var(--meter-yellow) 14%, transparent);
    border-bottom: var(--border-weight) solid var(--color-border);
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-text);
  }
  .body {
    flex: 1;
    min-height: 0;
    display: flex;
  }
  .subnav {
    flex: 0 0 240px;
    border-right: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
    padding: 10px 0;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
  }
  .nav-item {
    display: flex;
    flex-direction: column;
    border-left: 2.5px solid transparent;
  }
  .nav-item.active {
    background: color-mix(in srgb, var(--color-accent) 10%, transparent);
    border-left-color: var(--color-accent);
  }
  .nav-main {
    display: flex;
    align-items: center;
    gap: 8px;
    width: 100%;
    text-align: left;
    padding: 9px 14px 4px;
    background: transparent;
    border: 0;
    color: var(--color-dim);
    cursor: pointer;
  }
  .nav-item:hover .nav-main {
    color: var(--color-text);
  }
  .nav-item.active .nav-main {
    color: var(--color-accent);
  }
  .nav-name {
    flex: 1;
    min-width: 0;
    font-family: var(--font-ui);
    font-size: 12px;
    font-weight: 500;
    color: inherit;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .nav-item.active .nav-name {
    font-weight: 600;
  }
  .nav-badge {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 8px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--color-muted);
    border: var(--border-weight) solid var(--color-border);
    padding: 1px 4px;
  }
  .nav-actions {
    display: flex;
    gap: 6px;
    padding: 0 14px 9px;
  }
  .mini-btn {
    flex: 1;
    padding: 4px 6px;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-muted);
    cursor: pointer;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.04em;
    text-transform: uppercase;
  }
  .mini-btn:hover {
    color: var(--color-text);
    border-color: var(--color-accent);
  }
  .mini-btn.copied {
    color: var(--meter-green);
    border-color: var(--meter-green);
  }
  .addnav {
    margin: 8px 12px 4px;
    padding: 8px 10px;
    text-align: left;
    background: transparent;
    border: var(--border-weight) dashed var(--color-border);
    color: var(--color-dim);
    cursor: pointer;
    font-family: var(--font-ui);
    font-size: 12px;
  }
  .addnav:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }

  .pane {
    flex: 1;
    min-width: 0;
    min-height: 0;
    display: flex;
    flex-direction: column;
    padding: 16px 20px 20px;
  }
  .editor {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
    gap: 12px;
  }
  .editor-bar {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 10px;
    flex-wrap: wrap;
  }
  .name-in {
    flex: 0 0 220px;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 13px;
    font-weight: 500;
    padding: 7px 10px;
  }
  .name-in:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .tabs {
    display: flex;
  }
  .tab {
    padding: 7px 14px;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    border-left-width: 0;
    color: var(--color-dim);
    cursor: pointer;
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.06em;
    text-transform: uppercase;
  }
  .tab:first-child {
    border-left-width: var(--border-weight);
  }
  .tab:hover {
    color: var(--color-text);
  }
  .tab.on {
    background: color-mix(in srgb, var(--color-accent) 16%, transparent);
    color: var(--color-accent);
    border-color: var(--color-accent);
  }
  .editor-spacer {
    flex: 1;
  }
  .save-state {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .accent {
    padding: 7px 16px;
    background: var(--color-accent);
    border: 0;
    color: var(--color-accent-ink);
    cursor: pointer;
    font-family: var(--font-ui);
    font-size: 12px;
    font-weight: 600;
  }
  .ghost {
    padding: 7px 14px;
    background: none;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    cursor: pointer;
    font-family: var(--font-ui);
    font-size: 12px;
  }
  .ghost:hover {
    color: var(--color-text);
  }
  .ghost.danger:hover {
    color: var(--color-live);
    border-color: var(--color-live);
  }

  .editor-body {
    flex: 1;
    min-height: 0;
    display: flex;
  }
  .code-grid {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-columns: 1fr 1fr;
    grid-template-rows: 1fr 1fr;
    gap: 12px;
  }
  /* HTML spans the full top row; CSS + JS share the bottom row. */
  .code-cell:first-child {
    grid-column: 1 / -1;
  }
  .code-cell {
    display: flex;
    flex-direction: column;
    gap: 6px;
    min-height: 0;
    min-width: 0;
  }
  .cell-kicker {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .code-cell :global(.code-host) {
    flex: 1;
    min-height: 0;
  }
  .scroll-pane {
    flex: 1;
    min-height: 0;
    overflow: auto;
    padding-right: 4px;
  }
  .dim {
    color: var(--color-muted);
    margin: 0;
  }
  .err {
    margin: 0 0 12px;
    color: var(--color-live);
    font-size: 12px;
  }
</style>
