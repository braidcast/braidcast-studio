<script lang="ts">
  // Overlays page (master-detail): a left list of overlay widgets + a right editor
  // with a Simple (settings) / Advanced (html-css-js) mode toggle and a live preview.
  // Widgets are loopback-SSE overlays served by the C++ Overlay::Server; the user copies
  // a widget URL into an OBS Browser Source. Edits mutate a local $state copy and
  // debounce into overlays.update (~500ms), then bump reloadKey so the preview iframe
  // reloads with the freshly-assembled document. A Save button flushes immediately.
  // Create/Duplicate/Reset/Delete + the host's overlays.changed push keep the list in sync.
  //
  // A widget is STOCK (custom == null) or FORKED. A stock widget is served from the
  // shipped default-<type>/ template, so template fixes reach it; its Advanced pane is a
  // read-only view of that template plus the one control that forks it. A forked widget
  // serves its own code and stops tracking the template, which is why forking is an
  // explicit gesture rather than a side effect of typing in Advanced. Reset is the way
  // back: it discards the custom code and keeps the settings.
  import { onMount, onDestroy } from "svelte";
  import { obs, type OverlayListItem, type OverlayUpdateParams, type OverlayWidget } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { copyText } from "$lib/utils/clipboard";
  import CodeGrid, { type CodePart } from "$lib/overlays/CodeGrid.svelte";
  import FieldsPanel from "$lib/overlays/FieldsPanel.svelte";
  import { labelFor, WIDGET_TYPES } from "$lib/overlays/widgetTypes";
  import PreviewPane from "$lib/overlays/PreviewPane.svelte";
  import CollectionDialog, { type DialogSpec } from "$lib/dialogs/CollectionDialog.svelte";
  import PageShell from "$lib/ui/PageShell.svelte";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import Segmented, { type SegmentedOption } from "$lib/ui/Segmented.svelte";

  type PaneMode = "simple" | "advanced" | "preview";

  const MODE_OPTIONS: SegmentedOption[] = [
    { label: "Simple", value: "simple" },
    { label: "Advanced", value: "advanced" },
  ];
  const PREVIEW_OPTION: SegmentedOption = { label: "Preview", value: "preview" };
  // One label for the two buttons that start a reset, the confirm dialog's affirmative, and
  // the two sentences that quote it. Reworded in one place, because prose naming a button
  // that no longer says that is prose telling the user to look for something absent.
  const RESET_LABEL = "Reset code";

  // Below this the editor column can't hold a legible fields/code pane beside the
  // preview (the rail and the widget list already take 310px), so the preview stops
  // being a fixed column and becomes a selectable mode instead. Driving the layout
  // from this one query rather than from CSS media rules keeps the pane state and the
  // rendered columns from disagreeing at the boundary.
  const WIDE_PREVIEW_QUERY = "(min-width: 1100px)";

  let items = $state<OverlayListItem[]>([]);
  let selectedId = $state<string | null>(null);
  let widget = $state<OverlayWidget | null>(null);
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
  // True while overlays.resetDefaults is in flight. It is the re-entrancy guard for the
  // reset and the disabled state of the controls that start one: the confirm dialog's
  // button is never disabled and Enter commits from anywhere, and the host answers the
  // second reset AlreadyStock — a red banner reporting failure for an operation that
  // succeeded, with the first call's completion dropping the flag out from under the
  // second. It does NOT gate saving: a save raised inside the window queues behind the
  // mutation (runQueued) rather than standing down.
  let resetting = $state(false);
  // The same for overlays.fork, plus the "Customizing…" label on the button that started
  // it. Saving is likewise queued rather than refused, so an edit typed inside the window
  // still reaches the host.
  let forking = $state(false);

  const paneOptions = $derived(wide ? MODE_OPTIONS : [...MODE_OPTIONS, PREVIEW_OPTION]);
  const forked = $derived(!!widget?.custom);

  // A stock widget's code is on disk, not on the widget, so the read is keyed by TYPE.
  // Going through `stockType` rather than reading the widget directly is what keeps the
  // request stable: an overlays.changed refetch hands back a new widget object with the
  // same type, and asking again would tear down three CodeMirror instances under someone
  // reading them. Deriveds are pull-based, so nothing is fetched until the stock Advanced
  // pane actually renders, and the host answers from a per-process cache after the first.
  const stockType = $derived(widget && !widget.custom ? widget.type : null);
  const stockTemplate = $derived(stockType ? obs.call("overlays.template", { type: stockType }) : null);

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

  let saveTimer: ReturnType<typeof setTimeout> | undefined;
  let copiedTimer: ReturnType<typeof setTimeout> | undefined;
  // onDestroy has to be the last writer. Clearing saveTimer no longer covers that on its
  // own: a flush whose timer already fired can be waiting its turn behind a fork or reset,
  // and there is no timer left to cancel by the time the page goes.
  let destroyed = false;
  // Bumped by every edit. A save captures it when it starts writing and clears `dirty` only
  // if it still matches, so a keystroke that arrives while overlays.update is in flight —
  // which it will, since the host writes overlays.json synchronously inside that call —
  // keeps the flag and therefore keeps its trailing debounce. Clearing unconditionally is
  // what would report "Saved" for a value never sent and then let the next overlays.changed
  // echo revert it on screen.
  let saveGen = 0;

  // Every host mutation of the open document — the debounced PATCH, the fork, the reset,
  // the delete — runs through here, one at a time. Serializing them is what lets a save
  // raised inside a fork or reset window still reach the host instead of standing down: it
  // waits its turn and then builds its payload from whatever the mutation left behind,
  // rather than describing the document as it was before. They also apply their returned
  // `rev` in the order the host accepted them, so the local copy cannot be left behind the
  // stored one and read as an external edit on the next overlays.changed echo.
  //
  // Calling it from INSIDE a queued body deadlocks the chain for the rest of the session:
  // the inner call waits on a tail that cannot settle until the outer body it is running in
  // returns. Nothing does that today — fork and reset call flushSave BEFORE taking the
  // queue, never within it — and any new mutation has to keep it that way.
  let mutations: Promise<unknown> = Promise.resolve();
  function runQueued<T>(run: () => Promise<T>): Promise<T> {
    const next = mutations.then(run, run);
    // The tail is settled either way: a rejected one would hand its rejection to every
    // mutation queued after it.
    mutations = next.then(
      () => undefined,
      () => undefined,
    );
    return next;
  }

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
    // Flush any pending edit to the outgoing widget before switching. This also orders the
    // switch behind a fork or reset already holding the queue, so a mutation still sees the
    // selection it was started on.
    await flushSave();
    selectedId = id;
    pane = "simple";
    try {
      const gen = saveGen;
      const w = await obs.call("overlays.get", { id });
      // A newer select() may have superseded this one while the fetch was in flight;
      // don't overwrite the current selection with a stale widget.
      if (selectedId !== id) {
        return;
      }
      // Something was typed into the OUTGOING widget while this fetch was in flight. It is
      // still unsaved and still addressable — the buffer is swapped below, not above — so it
      // gets the same flush the entry above gives rather than being dropped on the swap.
      // Keeping `dirty` set instead would only make the flag describe a widget that is about
      // to be replaced, which saves nothing and then suppresses every external refetch.
      if (saveGen !== gen) {
        await flushSave();
        if (selectedId !== id) {
          return;
        }
      }
      widget = w;
      dirty = false;
      reloadKey++;
    } catch (e) {
      error = (e as Error).message;
      widget = null;
    }
  }

  function scheduleSave(): void {
    const w = widget;
    if (!w) {
      return;
    }
    dirty = true;
    saveGen++;
    // The banner is cleared on the gesture, never from inside the queued save: a save that
    // ran after a failed fork would otherwise wipe that fork's error one microtask after it
    // was set, before it was ever painted.
    error = null;
    clearTimeout(saveTimer);
    // Armed against an id rather than against whatever is open when it fires: a reset
    // replaces the widget object and a selection change replaces the widget outright, both
    // well inside 500ms.
    saveTimer = setTimeout(() => void flushSave(w.id), 500);
  }

  // The Save button's gesture. Separate from flushSave so the banner clear lands here, at
  // the click, rather than on the debounce or in the queued body.
  function saveNow(): void {
    error = null;
    void flushSave();
  }

  // The "did the server copy actually change?" test. Written as an explicit projection of
  // the persisted document rather than a stringify of the whole widget, because that fixes
  // the key order of the comparison itself. What it leaves out is left out on two different
  // grounds. `url` and `schema` are not persisted at all — `schema` is `custom.fields`
  // verbatim once forked and a process-constant while stock, so it could only restate what
  // is compared here anyway. `id`, `token` and `type` ARE persisted, but none of them can
  // change under a selection: a document answering to a different one is a different
  // widget, not an edit to this one. A persisted field that can change belongs in here.
  //
  // The sort is the part that has to be right. The host emits `settings` from a sorted map
  // while the local copy appends new keys last, so an unsorted compare would read our own
  // save echo as an external edit and reload the preview a second time on every save.
  function docJson(w: OverlayWidget): string {
    const settings = Object.entries(w.settings).sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0));
    return JSON.stringify({ name: w.name, rev: w.rev, settings, custom: w.custom, assets: w.assets });
  }

  // Writes the open document. `id` names the widget the unsaved edits belong to; the
  // payload is read from the live buffer and only while that is still the same widget, so
  // one overlay's pending edits can never be applied to another — there is no path through
  // here that reads the id from one object and the values from a second. Queued rather than
  // refused while a fork or reset holds the document, which is what keeps select()'s
  // flush-on-the-way-out a guarantee instead of a no-op inside those windows.
  async function flushSave(id = widget?.id): Promise<void> {
    clearTimeout(saveTimer);
    if (id === undefined) {
      return;
    }
    await runQueued(async () => {
      const w = widget;
      // Nothing unsaved, the page is gone, or the selection moved on while this waited its
      // turn. `dirty` is the whole test for "is there anything to write": every edit path
      // sets it and only a landed save or a fresh fetch clears it, so without it a plain
      // selection change would PATCH, bump `rev` host-side, and reload a preview for a
      // write nobody asked for.
      if (destroyed || !w || w.id !== id || !dirty) {
        return;
      }
      // Captured here rather than at arm time: the payload below is read from the live
      // buffer at this moment, so everything typed up to now is covered by this write and
      // only what arrives during the call itself has to survive it.
      const gen = saveGen;
      saving = true;
      try {
        // `settings` replaces the stored override set wholesale, so the whole map goes every
        // time. The code goes only for a forked widget: a stock one has none of its own, and
        // the host rejects an update that tries to give it some.
        const patch: OverlayUpdateParams = { id: w.id, name: w.name, settings: { ...w.settings } };
        if (w.custom) {
          patch.html = w.custom.html;
          patch.css = w.custom.css;
          patch.js = w.custom.js;
        }
        const saved = await obs.call("overlays.update", patch);
        // Level the local copy with the stored revision, so the overlays.changed echo this
        // save triggers compares equal and doesn't reload the preview a second time.
        w.rev = saved.rev;
        // Only what this call actually carried is saved. An edit that arrived while it was
        // in flight keeps the flag, so the debounce it armed still has something to write —
        // and it is cleared after the await, never before, or a rejected save would drop the
        // flag along with the work.
        if (saveGen === gen) {
          dirty = false;
        }
        reloadKey++;
        // Keep the list row's name label in sync without a full re-fetch.
        items = items.map((it) => (it.id === w.id ? { ...it, name: w.name } : it));
      } catch (e) {
        error = (e as Error).message;
      } finally {
        saving = false;
      }
    });
  }

  async function create(type: string, name: string): Promise<void> {
    typeMenuOpen = false;
    error = null;
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
    error = null;
    try {
      const w = await obs.call("overlays.duplicate", { id: selectedId });
      refresh();
      await select(w.id);
    } catch (e) {
      error = (e as Error).message;
    }
  }

  // Takes the widget off the shipped template and onto a copy of it. The response carries
  // the new `custom` outright, so this applies it in place instead of refetching: the whole
  // point of forking is that nothing the user configured changes, and swapping the widget
  // object would put every setting typed in the last half-second at the mercy of the race.
  async function forkCode(): Promise<void> {
    const w = widget;
    if (!w || w.custom || forking) {
      return;
    }
    // Raised before the first await, so a second click on a button the flush has not yet
    // disabled cannot start a second fork.
    forking = true;
    error = null;
    try {
      // Pending settings land first. The fork is a separate host mutation, and a debounced
      // PATCH firing after it would be describing the widget as it was before.
      await flushSave();
      // Identity, not id: an overlays.changed refetch can replace the object under the same
      // id, and writing `custom` onto the copy we no longer render would silently do nothing.
      if (widget !== w) {
        return;
      }
      const res = await runQueued(() => obs.call("overlays.fork", { id: w.id }));
      if (widget !== w) {
        return;
      }
      w.custom = res.custom;
      // A forked widget's schema IS its own field list; the fork seeded it from the type's,
      // so the form does not change shape here, only where its structure comes from.
      w.schema = res.custom.fields;
      w.rev = res.rev;
      reloadKey++;
    } catch (e) {
      error = (e as Error).message;
    } finally {
      forking = false;
    }
  }

  function confirmReset(): void {
    const target = widget;
    if (!target) {
      return;
    }
    dialog = {
      kind: "confirm",
      title: "Reset Overlay Code",
      message:
        `Discard the custom HTML, CSS and JS on "${target.name}" and go back to the built-in ` +
        `${labelFor(target.type)} template? The custom code cannot be recovered. Your settings, the name ` +
        `and uploaded assets are all kept, and the overlay starts receiving template improvements again.`,
      confirmLabel: RESET_LABEL,
      onCommit: () => void resetDefaults(target.id),
    };
  }

  // Clears `custom`, returning the widget to the on-disk template. `settings` survives, so
  // the pending debounced PATCH is flushed rather than dropped: those are values the reset
  // is documented to keep, and the code it also carries is about to be discarded anyway.
  // The refetch is explicit rather than left to the overlays.changed echo, so the editor
  // shows the returned document however the echo races. Both host calls hold the mutation
  // queue for the whole round trip, so a save raised inside the window is written against
  // the widget the reset left behind — stock, with no code on it for the host to refuse.
  //
  // `dirty` is global while this is per-widget, so it is only touched once the selection is
  // confirmed still ours: the user can switch overlays and start typing inside either
  // await, and clearing the flag then would strand THAT widget's edits. The host also
  // refuses a widget whose type has no template on disk, so failure is reachable and must
  // leave the flag alone as well — flushSave clears it only on a save that landed, same
  // rule.
  async function resetDefaults(id: string): Promise<void> {
    // The dialog's confirm button stays enabled and Enter commits from anywhere, so a
    // second commit is one keypress away. The host answers it AlreadyStock, which would
    // paint a red banner reporting failure for an operation that had just succeeded.
    if (resetting) {
      return;
    }
    resetting = true;
    error = null;
    try {
      await flushSave();
      // The reset itself is unconditional. The user asked for it on a widget the confirm
      // dialog named, so a selection that moved inside the flush must not turn a destructive
      // action into a silent no-op; only the local-state application below is conditional.
      await runQueued(async () => {
        await obs.call("overlays.resetDefaults", { id });
        const w = await obs.call("overlays.get", { id });
        if (selectedId !== id) {
          return;
        }
        // Values typed inside the round trip are not the reset's to discard — the confirm
        // dialog promises it keeps the settings AND the name, and it keeps both by
        // definition — so an unflushed buffer wins over the refetched copy for each of them.
        // Taking the server's `name` here is what let the following flush write the old name
        // back over a rename typed inside the window. Everything else, `custom` above all, is
        // what the reset just changed and has to come from the server.
        const unsaved = dirty && widget ? { name: widget.name, settings: { ...widget.settings } } : null;
        if (unsaved) {
          w.name = unsaved.name;
          w.settings = unsaved.settings;
        } else {
          dirty = false;
        }
        widget = w;
        reloadKey++;
      });
    } catch (e) {
      error = (e as Error).message;
    } finally {
      resetting = false;
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
    // (which would surface a spurious "no such overlay" error banner). Queued for the
    // second half of the same reason: a save whose timer already fired is waiting its turn
    // and no longer has a timer to cancel, so the delete has to line up behind it.
    clearTimeout(saveTimer);
    error = null;
    try {
      await runQueued(() => obs.call("overlays.delete", { id }));
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
    const failure = await copyText(item.url);
    if (failure !== null) {
      error = "Copy failed: " + failure;
      return;
    }
    copiedId = item.id;
    clearTimeout(copiedTimer);
    copiedTimer = setTimeout(() => (copiedId = null), 1400);
  }

  async function addToScene(item: OverlayListItem): Promise<void> {
    error = null;
    try {
      await obs.call("overlays.addToScene", { id: item.id });
    } catch (e) {
      error = (e as Error).message;
    }
  }

  // --- editor field bindings (mutate local widget, then debounce the update) ---
  // Only a forked widget has code to write to; the stock pane that shows the template is
  // read-only and never reaches here.
  function onCode(part: CodePart, v: string): void {
    if (widget?.custom) {
      widget.custom[part] = v;
      scheduleSave();
    }
  }
  function onSettings(next: Record<string, unknown>): void {
    if (widget) {
      widget.settings = next;
      scheduleSave();
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
      const local = widget;
      if (!local || docJson(w) !== docJson(local)) {
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
    destroyed = true;
    clearTimeout(saveTimer);
    clearTimeout(copiedTimer);
  });
</script>

<PageShell title="Overlays" sub="Loopback widgets · copy a URL into a browser source">

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
            {#if forked}
              <!-- Visible from every pane, because it is the reason this widget stopped
                   picking up template fixes and the Advanced pane is not always open. -->
              <span class="cv-badge cv-badge--default">Custom code</span>
            {/if}
            <span class="editor-spacer"></span>
            <span class="save-state">{saving ? "Saving…" : "Saved"}</span>
            <!-- Not held off during a fork or reset: this button's whole job is to save, and
                 the queue is what makes a save safe inside those windows. -->
            <button class="accent" disabled={saving} onclick={saveNow}>Save</button>
            <button class="ghost" onclick={duplicate}>Duplicate</button>
            <!-- A stock widget has no custom code to discard, so Reset would be a no-op. -->
            {#if forked}
              <button class="ghost" disabled={resetting} onclick={confirmReset}>{RESET_LABEL}</button>
            {/if}
            <button class="ghost danger" onclick={() => void confirmDelete()}>Delete</button>
          </div>

          <div class="editor-body" class:split={wide}>
            {#if wide || pane !== "preview"}
              <div class="edit-pane">
                {#if pane === "advanced"}
                  <!-- The two arms are mutually exclusive on purpose: CodePane fixes its
                       read-only state at mount, so forking has to arrive as a fresh grid. -->
                  <div class="code-stack">
                    {#if widget.custom}
                      <div class="code-note">
                        <p>
                          <b>This overlay runs your own code.</b> It no longer picks up improvements to the built-in
                          {labelFor(widget.type)} template. "{RESET_LABEL}" discards your code and puts it back on the
                          built-in template, keeping your settings.
                        </p>
                        <button class="ghost" disabled={resetting} onclick={confirmReset}>{RESET_LABEL}</button>
                      </div>
                      <CodeGrid
                        html={widget.custom.html}
                        css={widget.custom.css}
                        js={widget.custom.js}
                        onChange={onCode}
                      />
                    {:else}
                      <div class="code-note">
                        <p>
                          <b>This overlay uses the built-in {labelFor(widget.type)} template</b>, shown below, and picks
                          up improvements to it automatically. Customizing takes a private copy you can edit — from then
                          on this overlay stops receiving those improvements. Your settings are kept, and "{RESET_LABEL}"
                          puts it back on the built-in template.
                        </p>
                        <button class="accent" disabled={forking} onclick={() => void forkCode()}>
                          {forking ? "Customizing…" : "Customize code"}
                        </button>
                      </div>
                      {#if stockTemplate}
                        {#await stockTemplate}
                          <p class="tpl-state">Reading the built-in template…</p>
                        {:then template}
                          <CodeGrid html={template.html} css={template.css} js={template.js} readonly />
                        {:catch e}
                          <p class="tpl-state tpl-state--err">The built-in template can't be shown ({e.message}).</p>
                        {/await}
                      {/if}
                    {/if}
                  </div>
                {:else}
                  <div class="scroll-pane">
                    <FieldsPanel
                      schema={widget.schema}
                      settings={widget.settings}
                      widgetId={widget.id}
                      onChange={onSettings}
                    />
                  </div>
                {/if}
              </div>
            {/if}
            {#if wide || pane === "preview"}
              <div class="preview-pane">
                <PreviewPane url={widget.url} widgetId={widget.id} widgetType={widget.type} {reloadKey} />
              </div>
            {/if}
          </div>
        </div>
      {/if}
    </div>
  </div>
</PageShell>

{#if dialog}
  <CollectionDialog {...dialog} onClose={() => (dialog = null)} />
{/if}

<style>
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
  /* The global button:disabled only dims; without the guard the hover would still
     brighten a button that will not respond. */
  .ghost:hover:not(:disabled) {
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
  .code-stack {
    flex: 1;
    min-width: 0;
    min-height: 0;
    display: flex;
    flex-direction: column;
    gap: 12px;
  }
  /* The stock/forked notice: prose on the left, the one action it offers on the right. */
  .code-note {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 10px 12px;
    border: var(--border-weight) solid var(--color-border);
    border-left: 2px solid var(--color-accent);
    background: var(--color-surface);
  }
  .code-note p {
    flex: 1;
    min-width: 0;
    margin: 0;
    font-size: 11.5px;
    line-height: 1.5;
    color: var(--color-dim);
  }
  .code-note b {
    font-weight: 600;
    color: var(--color-text);
  }
  .code-note button {
    flex: 0 0 auto;
  }
  .tpl-state {
    flex: 1;
    margin: 0;
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
  }
  /* A modifier rather than the pane's .err class: that one is the banner at the top of the
     page and sets its own font-size and margin at the same specificity as .tpl-state, so
     sharing it made source order decide the size and left a margin on top of the stack's
     own gap. This line is a template-state line that happens to be an error. */
  .tpl-state--err {
    color: var(--color-live);
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
