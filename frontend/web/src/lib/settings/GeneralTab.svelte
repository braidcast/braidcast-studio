<script lang="ts">
  import { obs, type ChatHistoryRetention, type ChatHistoryStatus, type GeneralSettings } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { popEsc, pushEsc, isTopEsc } from "$lib/utils/escStack";
  import {
    RETENTION_OPTIONS,
    historyStatusText,
    isRetention,
    retentionChangeDeletes,
    retentionConfirmMessage,
    sessionOnlyHint,
  } from "./chatHistory";
  import { openMissingFiles } from "$lib/dialogs/missingFilesOpener.svelte";
  import { openLogViewer } from "$lib/dialogs/logViewerOpener.svelte";
  import { openImporter } from "$lib/dialogs/importerOpener.svelte";
  import { goLivePref, setGoLivePref } from "$lib/stores/goLivePrefStore.svelte";
  import { RequestGuard } from "$lib/utils/requestGuard";
  import { fxStore } from "$lib/stores/fxStore.svelte";
  import { automaticLabel, currencyChoices, fxStatusText } from "./fxSettings";
  import Button from "$lib/ui/Button.svelte";
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";

  // General app settings, live-applied (the page model has no Apply boundary):
  // each control change pushes only its changed key via settings.setGeneral and
  // reconciles from the returned full state. settings.generalChanged keeps this in
  // sync with any external edit.
  const multiviewLayouts: { label: string; value: string }[] = [
    { label: "Horizontal, Top", value: "horizontalTop" },
    { label: "Horizontal, Bottom", value: "horizontalBottom" },
    { label: "Vertical, Left", value: "verticalLeft" },
    { label: "Vertical, Right", value: "verticalRight" },
    { label: "Scenes Only (4)", value: "scenesOnly4" },
    { label: "Scenes Only (9)", value: "scenesOnly9" },
    { label: "Scenes Only (16)", value: "scenesOnly16" },
    { label: "Scenes Only (24)", value: "scenesOnly24" },
  ];

  const DEFAULTS: GeneralSettings = {
    projectorAlwaysOnTop: false,
    snapEnabled: true,
    snapDistance: 10,
    snapToEdge: true,
    snapToSource: true,
    snapToCenter: true,
    previewOverflow: "selection",
    previewOverflowInvisible: false,
    previewSafeAreas: false,
    previewSpacingHelpers: true,
    warnBeforeGoLive: false,
    warnBeforeStop: false,
    scheduleRequireAllDestinations: false,
    chatHistoryRetention: "off",
    fxHomeCurrency: "",
    startMinimized: false,
    minimizeToTray: false,
    alwaysShowTray: false,
    multiviewLayout: "horizontalTop",
    multiviewDrawNames: true,
    multiviewDrawSafeAreas: false,
    importerPrompts: true,
    scenesGridMode: false,
  };

  let s = $state<GeneralSettings>({ ...DEFAULTS });
  let loaded = $state(false);
  let error = $state<string | null>(null);
  const guard = new RequestGuard();

  // The state the engine last confirmed, which is what a rejected apply rolls back to.
  // Not the value the control saw: with two applies in flight that one already carries
  // the earlier patch, which may itself have been rejected.
  let confirmed: GeneralSettings = { ...DEFAULTS };

  function adopt(g: GeneralSettings): void {
    s = g;
    confirmed = { ...g };
    // Another window may have changed retention under a pending confirm; it is asked
    // only while the change it would make still deletes something.
    if (historyConfirm?.kind === "retention" && !retentionChangeDeletes(g.chatHistoryRetention, historyConfirm.to)) {
      historyConfirm = null;
    }
  }

  $effect(() => {
    let active = true;
    const current = guard.claim();
    obs
      .call("settings.getGeneral")
      .then((g) => {
        if (active && current()) adopt(g);
      })
      .catch((e) => {
        if (active) error = (e as Error).message;
      })
      .finally(() => {
        if (active) loaded = true;
      });
    const off = obs.on(EV.settingsGeneralChanged, (g) => {
      // The push is the engine's own account of what it holds, so it outranks every
      // reply still in flight -- including a revert about to restore an older value.
      guard.supersede();
      adopt(g);
    });
    return () => {
      active = false;
      off();
    };
  });

  // Optimistic local set, then reconcile from the echoed full state.
  fxStore.start();
  const FX_ID = "fx-home-currency";
  const fxLine = $derived(fxStatusText(fxStore.snapshot));

  async function apply(patch: Partial<GeneralSettings>): Promise<void> {
    error = null;
    const current = guard.claim();
    s = { ...s, ...patch };
    try {
      const g = await obs.call("settings.setGeneral", patch);
      if (current()) adopt(g);
    } catch (e) {
      error = (e as Error).message;
      if (current()) s = { ...confirmed };
    }
  }

  // Chat history. The status line is read on the async lane and refreshed after anything
  // that can change it: a retention change (ours or another window's) and a clear.
  let history = $state<ChatHistoryStatus | null>(null);
  let historyFailed = $state(false);
  const historyGuard = new RequestGuard();
  const historyLine = $derived(
    history
      ? historyStatusText(history)
      : { text: historyFailed ? "Could not read the chat history status." : "Reading status…", problem: historyFailed },
  );
  const sessionHint = $derived(history ? sessionOnlyHint(history.moderatedPlatforms) : "");

  function refreshHistory(): void {
    const current = historyGuard.claim();
    obs
      .call("chat.historyStatus")
      .then((h) => {
        if (!current()) return;
        history = h;
        historyFailed = false;
      })
      .catch(() => {
        if (!current()) return;
        history = null;
        historyFailed = true;
      });
  }

  $effect(() => {
    refreshHistory();
    let seenRetention: string | null = null;
    const offGeneral = obs.on(EV.settingsGeneralChanged, (g) => {
      if (g.chatHistoryRetention !== seenRetention) {
        seenRetention = g.chatHistoryRetention;
        refreshHistory();
      }
    });
    // chat.cleared reaches every window, this one included, so a clear made here refreshes
    // through it too.
    const offCleared = obs.on(EV.chatCleared, refreshHistory);
    return () => {
      offGeneral();
      offCleared();
    };
  });

  // A change that deletes stored chat waits here for its inline confirm; the select shows
  // the pending choice meanwhile. Only one confirm is open at a time.
  type HistoryConfirm = { kind: "retention"; to: ChatHistoryRetention } | { kind: "clear" };
  let historyConfirm = $state<HistoryConfirm | null>(null);
  let clearing = $state(false);
  const RETENTION_ID = "chat-history-retention";
  const CLEAR_ID = "chat-history-clear";
  const CONFIRM_MESSAGE_ID = "chat-history-confirm-message";

  const shownRetention = $derived(
    historyConfirm?.kind === "retention" ? historyConfirm.to : s.chatHistoryRetention,
  );
  // Said through a live region rather than by moving focus: arrow keys on a closed select
  // change its value at once, and taking focus away would stop the user mid-way.
  const confirmAnnouncement = $derived(
    historyConfirm?.kind === "retention" ? `${retentionConfirmMessage(historyConfirm.to)} Confirm or cancel below.` : "",
  );

  function chooseRetention(to: ChatHistoryRetention): void {
    if (retentionChangeDeletes(s.chatHistoryRetention, to)) {
      historyConfirm = { kind: "retention", to };
      return;
    }
    historyConfirm = null;
    void apply({ chatHistoryRetention: to });
  }

  function openClearConfirm(): void {
    if (!clearing) historyConfirm = { kind: "clear" };
  }

  // Clear is an explicit activation, so its strip opens on Cancel, the choice that deletes
  // nothing. The retention strip leaves focus on the select.
  function focusCancel(node: HTMLElement): void {
    node.querySelector<HTMLButtonElement>("[data-cancel]")?.focus();
  }

  // The strip owns Escape while it is open, wherever focus is, through the same stack
  // menus and dialogs use, so a layer opened over it still closes first. An Escape it takes
  // goes no further: App's window handler would otherwise also leave a preview group, since
  // it is gated on the preview's overlays rather than on this stack, and the stack may
  // already be popped by the time the event reaches the window.
  $effect(() => {
    if (!historyConfirm) return;
    const token = pushEsc();
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== "Escape" || !isTopEsc(token)) return;
      e.stopPropagation();
      closeConfirm();
    };
    document.addEventListener("keydown", onKey);
    return () => {
      document.removeEventListener("keydown", onKey);
      popEsc(token);
    };
  });

  // Focus inside the strip goes back to the control that opened it, which unmounting the
  // strip would lose; focus anywhere else stays where it is.
  function closeConfirm(): void {
    const opener = historyConfirm?.kind === "clear" ? CLEAR_ID : RETENTION_ID;
    const inStrip = (document.activeElement as HTMLElement | null)?.closest(".confirm") != null;
    historyConfirm = null;
    if (inStrip) document.getElementById(opener)?.focus();
  }

  async function commitConfirm(): Promise<void> {
    const pending = historyConfirm;
    closeConfirm();
    if (pending?.kind === "retention") {
      await apply({ chatHistoryRetention: pending.to });
    } else if (pending?.kind === "clear") {
      // Clear stays enabled, so the focus closeConfirm returned to it is not lost; a second
      // press while this runs opens nothing.
      error = null;
      clearing = true;
      try {
        await obs.call("chat.clear");
      } catch (e) {
        error = (e as Error).message;
      } finally {
        clearing = false;
      }
    }
  }
</script>

{#if !loaded}
  <p class="dim">Loading settings…</p>
{:else}
  <section class="group">
    <h4>Snapping</h4>

    <label class="check">
      <ToggleSwitch size="sm" checked={s.snapEnabled} onchange={(v) => void apply({ snapEnabled: v })} />
      Enable snapping
    </label>

    <div class="field">
      <span class="flabel">Snap distance (px)</span>
      <input
        class="num"
        type="number"
        min="0"
        max="100"
        step="1"
        disabled={!s.snapEnabled}
        value={s.snapDistance}
        onchange={(e) =>
          void apply({ snapDistance: Number.isFinite(e.currentTarget.valueAsNumber) ? e.currentTarget.valueAsNumber : s.snapDistance })}
      />
    </div>

    <label class="check" class:dis={!s.snapEnabled}>
      <ToggleSwitch
        size="sm"
        disabled={!s.snapEnabled}
        checked={s.snapToEdge}
        onchange={(v) => void apply({ snapToEdge: v })}
      />
      Snap to screen edges
    </label>
    <label class="check" class:dis={!s.snapEnabled}>
      <ToggleSwitch
        size="sm"
        disabled={!s.snapEnabled}
        checked={s.snapToSource}
        onchange={(v) => void apply({ snapToSource: v })}
      />
      Snap to other sources
    </label>
    <label class="check" class:dis={!s.snapEnabled}>
      <ToggleSwitch
        size="sm"
        disabled={!s.snapEnabled}
        checked={s.snapToCenter}
        onchange={(v) => void apply({ snapToCenter: v })}
      />
      Snap to center
    </label>

    <p class="dim hint">Changes apply immediately.</p>
  </section>

  <section class="group">
    <h4>Projectors</h4>
    <label class="check">
      <ToggleSwitch
        size="sm"
        checked={s.projectorAlwaysOnTop}
        onchange={(v) => void apply({ projectorAlwaysOnTop: v })}
      />
      Make projectors always on top
    </label>
  </section>

  <section class="group">
    <h4>Streaming</h4>
    <label class="check">
      <ToggleSwitch size="sm" checked={goLivePref.askStreamInfo} onchange={(v) => setGoLivePref("askStreamInfo", v)} />
      Ask for stream info on Go Live
    </label>
    <p class="dim note">Open the Stream Information panel when going live. When off, Go Live starts instantly with the last-saved metadata.</p>
    <label class="check">
      <ToggleSwitch
        size="sm"
        checked={s.warnBeforeGoLive}
        onchange={(v) => void apply({ warnBeforeGoLive: v })}
      />
      Warn before going live
    </label>
    <label class="check">
      <ToggleSwitch
        size="sm"
        checked={s.warnBeforeStop}
        onchange={(v) => void apply({ warnBeforeStop: v })}
      />
      Warn before stopping the stream
    </label>
  </section>

  {#snippet confirmStrip(message: string, action: string)}
    <p id={CONFIRM_MESSAGE_ID} class="confirm-msg">{message}</p>
    <Button size="sm" tone="live" onclick={() => void commitConfirm()}>{action}</Button>
    <Button size="sm" data-cancel onclick={closeConfirm}>Cancel</Button>
  {/snippet}

  <section class="group">
    <h4>Chat history</h4>
    <div class="field">
      <label class="flabel" for={RETENTION_ID}>Keep chat history</label>
      <select
        id={RETENTION_ID}
        value={shownRetention}
        aria-describedby={historyConfirm?.kind === "retention" ? CONFIRM_MESSAGE_ID : undefined}
        onchange={(e) => chooseRetention(e.currentTarget.value as ChatHistoryRetention)}
      >
        {#if !isRetention(s.chatHistoryRetention)}
          <!-- A newer build's value, or a hand edit: shown, but not one to choose. -->
          <option value={s.chatHistoryRetention} disabled>Not recognized</option>
        {/if}
        {#each RETENTION_OPTIONS as o (o.value)}
          <option value={o.value}>{o.label}</option>
        {/each}
      </select>
    </div>
    <p class="sr-only" aria-live="polite">{confirmAnnouncement}</p>
    {#if historyConfirm?.kind === "retention"}
      <div class="confirm" role="group" aria-labelledby={CONFIRM_MESSAGE_ID}>
        {@render confirmStrip(
          retentionConfirmMessage(historyConfirm.to),
          historyConfirm.to === "off" ? "Turn off and delete" : "Delete older chat",
        )}
      </div>
    {/if}
    <p class="dim note">Stored chat lets the chat dock scroll back past its last 1,000 messages and across restarts. This session keeps chat until Braidcast closes; after a crash it stays on disk until the next launch. 7 days keeps up to 10,000 messages.</p>
    {#if sessionHint && s.chatHistoryRetention === "7d"}<p class="dim note">{sessionHint}</p>{/if}
    <p class="note status" class:dim={!historyLine.problem} class:warn={historyLine.problem} role="status">{historyLine.text}</p>
    <div class="actions">
      <Button id={CLEAR_ID} onclick={openClearConfirm}>Clear chat history…</Button>
    </div>
    {#if historyConfirm?.kind === "clear"}
      <div class="confirm" role="group" aria-labelledby={CONFIRM_MESSAGE_ID} use:focusCancel>
        {@render confirmStrip("Delete all stored chat history and empty the chat docks?", "Delete")}
      </div>
    {/if}
    <p class="dim note">Deleting is best effort: deleted chat is overwritten in the history file, but the drive (an SSD especially) or the database journal can keep a copy for a while.</p>
  </section>

  <section class="group">
    <h4>Super Chat amounts</h4>
    <div class="field">
      <label class="flabel" for={FX_ID}>Your currency</label>
      <select id={FX_ID} value={s.fxHomeCurrency} onchange={(e) => void apply({ fxHomeCurrency: e.currentTarget.value })}>
        <option value="">{automaticLabel(fxStore.snapshot, s.fxHomeCurrency)}</option>
        {#each currencyChoices(fxStore.snapshot, s.fxHomeCurrency) as code (code)}
          <option value={code}>{code}</option>
        {/each}
      </select>
    </div>
    <p class="dim note">
      Super Chats show in your currency first and the viewer's after, in the chat and Events docks and on your
      overlays. The figure is approximate: YouTube pays out after its own fees and conversion.
    </p>
    <p class="note status" class:dim={!fxLine.problem} class:warn={fxLine.problem} role="status">{fxLine.text}</p>
  </section>

  <section class="group">
    <h4>Schedule</h4>
    <label class="check">
      <ToggleSwitch
        size="sm"
        checked={s.scheduleRequireAllDestinations}
        onchange={(v) => void apply({ scheduleRequireAllDestinations: v })}
      />
      Require every destination before starting
    </label>
    <p class="dim note">A scheduled stream will not start unless every destination it lists can go live. When off, it starts with whichever ones can and leaves the rest out.</p>
  </section>

  <section class="group">
    <h4>System Tray</h4>
    <label class="check">
      <ToggleSwitch
        size="sm"
        checked={s.minimizeToTray}
        onchange={(v) => void apply({ minimizeToTray: v })}
      />
      Minimize to system tray
    </label>
    <label class="check">
      <ToggleSwitch
        size="sm"
        checked={s.startMinimized}
        onchange={(v) => void apply({ startMinimized: v })}
      />
      Start minimized
    </label>
    <label class="check">
      <ToggleSwitch
        size="sm"
        checked={s.alwaysShowTray}
        onchange={(v) => void apply({ alwaysShowTray: v })}
      />
      Always show tray icon
    </label>
    <p class="dim note">Applies when the system tray is enabled.</p>
  </section>

  <section class="group">
    <h4>Multiview</h4>
    <div class="field">
      <span class="flabel">Layout</span>
      <select value={s.multiviewLayout} onchange={(e) => void apply({ multiviewLayout: e.currentTarget.value })}>
        {#each multiviewLayouts as l (l.value)}
          <option value={l.value}>{l.label}</option>
        {/each}
      </select>
    </div>
    <label class="check">
      <ToggleSwitch
        size="sm"
        checked={s.multiviewDrawNames}
        onchange={(v) => void apply({ multiviewDrawNames: v })}
      />
      Draw scene names
    </label>
    <label class="check">
      <ToggleSwitch
        size="sm"
        checked={s.multiviewDrawSafeAreas}
        onchange={(v) => void apply({ multiviewDrawSafeAreas: v })}
      />
      Draw safe areas
    </label>
    <p class="dim note">Applies to the Multiview window.</p>
  </section>

  <section class="group">
    <h4>Importer</h4>
    <label class="check">
      <ToggleSwitch
        size="sm"
        checked={s.importerPrompts}
        onchange={(v) => void apply({ importerPrompts: v })}
      />
      Prompt to import from other software
    </label>
    <p class="dim note">Used by the OBS Studio importer.</p>
    <Button onclick={() => openImporter()}>Import from OBS Studio…</Button>
    <p class="dim note">Bring scene collections, stream destinations, and video/audio settings in from an OBS Studio install.</p>
  </section>

  <section class="group">
    <h4>Sources</h4>
    <Button onclick={() => openMissingFiles()}>Find Missing Files…</Button>
    <p class="dim note">Locate and relink sources whose media file has moved or been renamed.</p>
  </section>

  <section class="group">
    <h4>Diagnostics</h4>
    <Button onclick={() => openLogViewer()}>View Current Log</Button>
    <p class="dim note">Show the current session log for troubleshooting and bug reports.</p>
  </section>

  {#if error}<p class="error" role="alert">{error}</p>{/if}
{/if}

<style>
  .group {
    padding: 12px 0;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .group:last-child {
    border-bottom: none;
  }
  .group h4 {
    margin: 0 0 10px;
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--color-dim);
  }
  .field {
    margin-bottom: 12px;
  }
  .flabel {
    display: block;
    font-size: 12px;
    color: var(--color-dim);
    margin-bottom: 6px;
  }
  .check {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 8px;
    font-size: 13px;
    color: var(--color-text);
    cursor: pointer;
  }
  .check.dis {
    color: var(--color-muted);
    cursor: default;
  }
  .num,
  select {
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    padding: 7px 10px;
    color: var(--color-text);
    font: inherit;
    width: 100%;
    max-width: 320px;
  }
  .num {
    max-width: 120px;
  }
  .num:focus,
  select:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .num:disabled {
    color: var(--color-muted);
  }
  .dim {
    color: var(--color-muted);
    margin: 0;
  }
  .hint {
    font-size: 12px;
    margin-top: 8px;
  }
  .note {
    font-size: 12px;
    margin-top: 8px;
  }
  .status.warn {
    color: var(--color-warn);
  }
  .actions {
    margin-top: 10px;
  }
  /* The inline confirm for a change that deletes stored chat: edged in the destructive
     tone its action button carries, so the strip reads as the warning it is. */
  .confirm {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 8px;
    max-width: 480px;
    margin: 0 0 8px;
    padding: 8px 10px;
    border: var(--border-weight) solid var(--color-live);
    background: var(--color-surface);
  }
  .confirm-msg {
    flex: 1 1 100%;
    margin: 0;
    font-size: 12px;
    color: var(--color-text);
  }
  .error {
    color: var(--color-live-text);
    margin: 6px 0 0;
    font-size: 12px;
  }
</style>
