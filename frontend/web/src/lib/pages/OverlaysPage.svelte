<script lang="ts">
  // Overlays page (master-detail): a left list of overlay widgets + a right editor
  // with a Simple (fields) / Advanced (html-css-js) mode toggle and a live preview.
  // Widgets are loopback-SSE overlays served by the C++ Overlay::Server; the user copies
  // a widget URL into an OBS Browser Source. Edits mutate a local $state copy and
  // debounce into overlays.update (~500ms), then bump reloadKey so the preview iframe
  // reloads with the freshly-assembled document. A Save button flushes immediately.
  // Create/Duplicate/Delete + the host's overlays.changed push keep the list in sync.
  import { onMount, onDestroy } from "svelte";
  import { obs, type OverlayListItem, type OverlayWidget, type OverlayField } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import CodePane from "$lib/overlays/CodePane.svelte";
  import FieldsDesigner from "$lib/overlays/FieldsDesigner.svelte";
  import PreviewPane from "$lib/overlays/PreviewPane.svelte";
  import CollectionDialog, { type DialogSpec } from "$lib/dialogs/CollectionDialog.svelte";
  import PageHeader from "$lib/ui/PageHeader.svelte";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import Segmented, { type SegmentedOption } from "$lib/ui/Segmented.svelte";

  type PaneMode = "simple" | "advanced" | "preview";

  const MODE_OPTIONS: SegmentedOption[] = [
    { label: "Simple", value: "simple" },
    { label: "Advanced", value: "advanced" },
  ];
  const PREVIEW_OPTION: SegmentedOption = { label: "Preview", value: "preview" };

  // Below this the editor column can't hold a legible fields/code pane beside the
  // preview (the rail and the widget list already take 310px), so the preview stops
  // being a fixed column and becomes a selectable mode instead. Driving the layout
  // from this one query rather than from CSS media rules keeps the pane state and the
  // rendered columns from disagreeing at the boundary.
  const WIDE_PREVIEW_QUERY = "(min-width: 1100px)";

  let items = $state<OverlayListItem[]>([]);
  let selectedId = $state<string | null>(null);
  let widget = $state<OverlayWidget | null>(null);
  // The selected widget's type schema. A stock widget stores only the values it
  // overrode, so this is what says which settings exist at all.
  let schema = $state<OverlayField[]>([]);
  // Schemas are per type and read from files the build stages, so they cannot change
  // while the app runs -- fetch each one once.
  const schemaCache = new Map<string, OverlayField[]>();
  let pane = $state<PaneMode>("simple");
  let wide = $state(false);
  let reloadKey = $state(0);
  let portChanged = $state(false);
  let serverDown = $state(false);
  let loaded = $state(false);
  let error = $state<string | null>(null);
  let copiedId = $state<string | null>(null);
  let saving = $state(false);
  let dialog = $state<DialogSpec | null>(null);
  let typeMenuOpen = $state(false);
  // True while the local editor buffer has edits not yet flushed to the backend, so
  // an external overlays.changed refetch never clobbers in-flight typing.
  let dirty = $state(false);

  const paneOptions = $derived(wide ? MODE_OPTIONS : [...MODE_OPTIONS, PREVIEW_OPTION]);

  // The settings form is the type's schema with this widget's overrides laid over it, so
  // the fields a stock widget shows always come from the template rather than from
  // anything the widget stored.
  const settingsFields = $derived<OverlayField[]>(
    schema.map((f) => ({ ...f, value: widget?.settings[f.key] ?? f.default })),
  );

  $effect(() => {
    const mq = window.matchMedia(WIDE_PREVIEW_QUERY);
    wide = mq.matches;
    const sync = (e: MediaQueryListEvent): void => {
      wide = e.matches;
    };
    mq.addEventListener("change", sync);
    return () => mq.removeEventListener("change", sync);
  });

  // Widening past the breakpoint retires the Preview cell, so a selection made while
  // narrow would otherwise leave the toggle pointing at an option that no longer exists.
  $effect(() => {
    if (wide && pane === "preview") {
      pane = "simple";
    }
  });

  // The widget types a user can create. Adding one is a single row here, not a new
  // branch: label + backend type + the default name a fresh widget gets.
  const WIDGET_TYPES: { type: string; label: string; name: string }[] = [
    { type: "alertbox", label: "Alert Box", name: "New Alert Box" },
    { type: "chatbox", label: "Chat Box", name: "New Chat Box" },
    { type: "ticker", label: "Event Ticker", name: "New Event Ticker" },
    { type: "goalbar", label: "Goal Bar", name: "New Goal Bar" },
    { type: "labels", label: "Label", name: "New Label" },
    { type: "viewercount", label: "Viewer Count", name: "New Viewer Count" },
    { type: "followercount", label: "Follower Count", name: "New Follower Count" },
    { type: "uptime", label: "Stream Uptime", name: "New Stream Uptime" },
    { type: "wheretowatch", label: "Where to Watch", name: "New Where to Watch" },
    { type: "chatleaderboard", label: "Chat Leaderboard", name: "New Chat Leaderboard" },
  ];

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
    pane = "simple";
    try {
      const w = await obs.call("overlays.get", { id });
      // A newer select() may have superseded this one while the fetch was in flight;
      // don't overwrite the current selection with a stale widget.
      if (selectedId !== id) {
        return;
      }
      widget = w;
      dirty = false;
      reloadKey++;
      await loadSchema(w.type);
    } catch (e) {
      error = (e as Error).message;
      widget = null;
    }
  }

  async function loadSchema(type: string): Promise<void> {
    const cached = schemaCache.get(type);
    if (cached) {
      schema = cached;
      return;
    }
    try {
      const r = await obs.call("overlays.schema", { type });
      schemaCache.set(type, r.schema);
      schema = r.schema;
    } catch (e) {
      // A type with no readable schema still edits fine through custom code, so this
      // costs the settings form rather than the whole widget.
      error = (e as Error).message;
      schema = [];
    }
  }

  function scheduleSave(): void {
    if (!widget) {
      return;
    }
    dirty = true;
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
      // custom is sent only when the widget is forked. Sending null here would read as
      // "revert to stock" and silently discard a fork; reverting goes through
      // overlays.setCustom, which is the one place that decision is made.
      const saved = await obs.call("overlays.update", {
        id: w.id,
        name: w.name,
        settings: w.settings,
        ...(w.custom ? { custom: w.custom } : {}),
      });
      // Level the local copy with the stored revision, so the overlays.changed echo this
      // save triggers compares equal and doesn't reload the preview a second time.
      w.rev = saved.rev;
      dirty = false;
      reloadKey++;
      // Keep the list row's name label in sync without a full re-fetch.
      items = items.map((it) => (it.id === w.id ? { ...it, name: w.name } : it));
    } catch (e) {
      error = (e as Error).message;
    } finally {
      saving = false;
    }
  }

  async function create(type: string, name: string): Promise<void> {
    typeMenuOpen = false;
    try {
      const w = await obs.call("overlays.create", { name, type });
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

  // Sources bound to a deleted widget go blank rather than disappear, so the count has to
  // be said out loud here — the alternative is finding out on stream. A failed count
  // falls back to the plain warning instead of blocking the delete.
  async function confirmDelete(): Promise<void> {
    const target = widget;
    if (!target) {
      return;
    }
    let inUse = 0;
    try {
      inUse = (await obs.call("overlays.usage", { id: target.id })).sources;
    } catch {
      inUse = 0;
    }
    const used = inUse > 0 ? ` It is used in ${inUse} source${inUse === 1 ? "" : "s"}, which will go blank.` : "";
    dialog = {
      kind: "confirm",
      title: "Delete Overlay",
      message: `Delete "${target.name}"?${used} This removes the widget and its uploaded assets.`,
      confirmLabel: "Delete",
      onCommit: () => void remove(target.id),
    };
  }

  async function remove(id: string): Promise<void> {
    // Cancel any pending debounced save so we don't PATCH a just-deleted id
    // (which would surface a spurious "no such overlay" error banner).
    clearTimeout(saveTimer);
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
  // The code panes only exist while the widget is forked, so each guards on custom
  // rather than assuming it: a stock widget has no copy of the template to edit.
  function onHtml(v: string): void {
    if (widget?.custom) {
      widget.custom.html = v;
      scheduleSave();
    }
  }
  function onCss(v: string): void {
    if (widget?.custom) {
      widget.custom.css = v;
      scheduleSave();
    }
  }
  function onJs(v: string): void {
    if (widget?.custom) {
      widget.custom.js = v;
      scheduleSave();
    }
  }
  function onFields(f: OverlayField[]): void {
    if (widget?.custom) {
      widget.custom.fields = f;
      scheduleSave();
    }
  }
  // A stock widget stores overrides only, so a value equal to the schema default is
  // OMITTED rather than written. Keeping it would pin the setting, and the widget would
  // stop following a template that later changes that default.
  function onSettingsFields(next: OverlayField[]): void {
    if (!widget) {
      return;
    }
    const settings: Record<string, unknown> = {};
    for (const f of next) {
      const def = schema.find((s) => s.key === f.key);
      if (!def || JSON.stringify(f.value) !== JSON.stringify(def.default)) {
        settings[f.key] = f.value;
      }
    }
    widget.settings = settings;
    scheduleSave();
  }

  async function confirmRevertToStock(): Promise<void> {
    dialog = {
      kind: "confirm",
      title: "Discard Custom Code",
      message: `"${widget?.name ?? ""}" goes back to the stock template for its type. The HTML, CSS, JS and custom fields you added are deleted and cannot be recovered.`,
      confirmLabel: "Discard",
      onCommit: () => void setCustom(false),
    };
  }

  // Forking and reverting both happen host-side so the template snapshot is atomic; the
  // reply carries the new widget, since custom has changed shape.
  async function setCustom(enabled: boolean): Promise<void> {
    if (!widget) {
      return;
    }
    await flushSave();
    const id = widget.id;
    try {
      await obs.call("overlays.setCustom", { id, enabled });
      const w = await obs.call("overlays.get", { id });
      if (selectedId === id) {
        widget = w;
        dirty = false;
        pane = enabled ? "advanced" : "simple";
        reloadKey++;
      }
    } catch (e) {
      error = (e as Error).message;
    }
  }
  function onName(v: string): void {
    if (widget) {
      widget.name = v;
      scheduleSave();
    }
  }

  // A widget changed somewhere (create/update/duplicate/delete, or a test). The
  // payload carries no id, so re-run the list and — per the bridge contract —
  // re-fetch the open widget so external edits show. Skip the refetch while the
  // local buffer is dirty (a pending debounced save) so we never clobber unsaved
  // edits; and only swap when the server copy actually differs, so our own save
  // echo doesn't force a needless preview reload.
  async function onOverlaysChanged(): Promise<void> {
    refresh();
    const id = selectedId;
    if (!id || dirty) {
      return;
    }
    try {
      const w = await obs.call("overlays.get", { id });
      if (selectedId !== id || dirty) {
        return;
      }
      if (JSON.stringify(w) !== JSON.stringify(widget)) {
        widget = w;
        reloadKey++;
      }
    } catch {
      // vanished/transient; refresh() already dropped a stale selection.
    }
  }

  onMount(() => {
    refresh();
    obs
      .call("overlays.serverInfo")
      .then((s) => {
        portChanged = !!s?.portChanged;
        serverDown = s ? !s.listening : false;
      })
      // Both banners default to hidden, so a failed read would otherwise claim a
      // healthy server on the exact page whose job is to report it.
      .catch((e) => (error = (e as Error).message));
    return obs.on(EV.overlaysChanged, onOverlaysChanged);
  });

  onDestroy(() => {
    clearTimeout(saveTimer);
    clearTimeout(copiedTimer);
  });
</script>

<div class="page">
  <PageHeader title="Overlays" sub="loopback widgets · copy a URL into a browser source" />

  {#if serverDown}
    <div class="banner down">Overlay server isn't running — widget URLs won't load in a Browser Source.</div>
  {/if}
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
      <div class="addwrap">
        <button class="addnav" aria-haspopup="menu" aria-expanded={typeMenuOpen} onclick={() => (typeMenuOpen = !typeMenuOpen)}>
          <Icon name="plus" size={12} /> New overlay
        </button>
        {#if typeMenuOpen}
          <div class="typemenu" role="menu">
            {#each WIDGET_TYPES as t (t.type)}
              <button class="typeopt" role="menuitem" onclick={() => void create(t.type, t.name)}>{t.label}</button>
            {/each}
          </div>
        {/if}
      </div>
    </nav>

    <div class="pane">
      {#if error}<p class="err">{error}</p>{/if}

      {#if !loaded}
        <EmptyState title="Loading overlays…" />
      {:else if !widget}
        <EmptyState title="No overlay selected" sub="Create one, or pick a widget from the list." />
      {:else}
        <div class="editor">
          <div class="editor-bar">
            <input
              class="name-in"
              value={widget.name}
              aria-label="Overlay name"
              oninput={(e) => onName(e.currentTarget.value)}
            />
            <Segmented options={paneOptions} value={pane} onChange={(v) => (pane = v as PaneMode)} />
            <span class="editor-spacer"></span>
            <span class="save-state">{saving ? "Saving…" : "Saved"}</span>
            <button class="accent" disabled={saving} onclick={() => void flushSave()}>Save</button>
            <button class="ghost" onclick={duplicate}>Duplicate</button>
            <button class="ghost danger" onclick={() => void confirmDelete()}>Delete</button>
          </div>

          <div class="editor-body" class:split={wide}>
            {#if wide || pane !== "preview"}
              <div class="edit-pane">
                {#if pane === "advanced"}
                  {#if widget.custom}
                    <div class="code-grid">
                      <div class="code-cell">
                        <span class="cell-kicker">HTML</span>
                        <CodePane value={widget.custom.html} lang="html" onChange={onHtml} />
                      </div>
                      <div class="code-cell">
                        <span class="cell-kicker">CSS</span>
                        <CodePane value={widget.custom.css} lang="css" onChange={onCss} />
                      </div>
                      <div class="code-cell">
                        <span class="cell-kicker">JS</span>
                        <CodePane value={widget.custom.js} lang="javascript" onChange={onJs} />
                      </div>
                      <div class="code-cell">
                        <span class="cell-kicker">Custom fields</span>
                        <div class="scroll-pane">
                          <FieldsDesigner
                            fields={widget.custom.fields}
                            widgetId={widget.id}
                            onChange={onFields}
                          />
                        </div>
                      </div>
                    </div>
                  {:else}
                    <div class="scroll-pane">
                      <div class="fork-note">
                        <p>
                          This is a stock {widget.type}. It renders from the template the app ships, so
                          it picks up improvements to that template automatically.
                        </p>
                        <p>
                          Enabling custom code copies that template into this widget and hands it over
                          to you. From then on this widget stops tracking the app's version.
                        </p>
                        <button class="accent" onclick={() => void setCustom(true)}>
                          Enable custom code
                        </button>
                      </div>
                    </div>
                  {/if}
                {:else}
                  <div class="scroll-pane">
                    {#if widget.custom}
                      <div class="fork-note">
                        <p>This widget uses custom code, so its settings are the fields you defined.</p>
                        <button class="ghost danger" onclick={() => void confirmRevertToStock()}>
                          Discard custom code
                        </button>
                      </div>
                      <FieldsDesigner
                        fields={widget.custom.fields}
                        widgetId={widget.id}
                        designable={false}
                        onChange={onFields}
                      />
                    {:else}
                      <FieldsDesigner
                        fields={settingsFields}
                        widgetId={widget.id}
                        designable={false}
                        onChange={onSettingsFields}
                      />
                    {/if}
                  </div>
                {/if}
              </div>
            {/if}
            {#if wide || pane === "preview"}
              <div class="preview-pane">
                <PreviewPane url={widget.url} widgetId={widget.id} {reloadKey} />
              </div>
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
  .banner {
    flex: 0 0 auto;
    padding: 8px 24px;
    background: color-mix(in srgb, var(--meter-yellow) 14%, transparent);
    border-bottom: var(--border-weight) solid var(--color-border);
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-text);
  }
  .banner.down {
    background: color-mix(in srgb, var(--color-live) 14%, transparent);
    color: var(--color-live);
  }
  .body {
    flex: 1;
    min-height: 0;
    display: flex;
  }
  .fork-note {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 10px;
    padding: 14px 16px;
    margin-bottom: 18px;
    border: var(--border-weight) solid var(--color-border);
    border-radius: 6px;
    background: var(--color-surface);
  }
  .fork-note p {
    margin: 0;
    font-size: 12px;
    line-height: 1.5;
    color: var(--color-text-dim);
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
    display: inline-flex;
    align-items: center;
    gap: 6px;
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
  .addwrap {
    position: relative;
    display: flex;
    flex-direction: column;
  }
  .addnav {
    width: auto;
  }
  .typemenu {
    display: flex;
    flex-direction: column;
    margin: 0 12px;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
  }
  .typeopt {
    text-align: left;
    padding: 8px 12px;
    background: transparent;
    border: 0;
    border-bottom: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    cursor: pointer;
    font-family: var(--font-ui);
    font-size: 12px;
  }
  .typeopt:last-child {
    border-bottom: 0;
  }
  .typeopt:hover {
    background: color-mix(in srgb, var(--color-accent) 12%, transparent);
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
    gap: 12px;
  }
  .edit-pane,
  .preview-pane {
    flex: 1 1 auto;
    min-width: 0;
    min-height: 0;
    display: flex;
  }
  .editor-body.split .preview-pane {
    flex: 0 0 clamp(300px, 34%, 520px);
  }
  .preview-pane > :global(.preview) {
    flex: 1;
    min-width: 0;
  }
  .code-grid {
    flex: 1;
    min-width: 0;
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
  .err {
    margin: 0 0 12px;
    color: var(--color-live);
    font-size: 12px;
  }
</style>
