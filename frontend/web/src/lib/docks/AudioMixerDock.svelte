<script lang="ts">
  import { obs, type AudioSource, type AudioMonitoringType } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { openFilters } from "$lib/dialogs/filterDialogOpener.svelte";
  import { openAdvAudio } from "$lib/dialogs/advAudioOpener.svelte";
  import ContextMenu, { type ContextMenuItem } from "$lib/menus/ContextMenu.svelte";
  import { clipboard } from "$lib/stores/clipboardStore.svelte";
  import PropertiesModal from "$lib/properties/PropertiesModal.svelte";
  import Icon from "$lib/ui/Icon.svelte";

  // Per-source faders + live dB meters. Levels arrive on the audio.levels push
  // (~30 Hz x N sources); we coalesce into a Map and flush once per animation
  // frame so layout is touched at most once a frame. The meter honors the
  // --meter-style token (segmented for Industrial, gradient for Graphite/Slate)
  // via :root[data-meter-style] (visual-style.html).
  // The mount adapter strips internal __* keys; this dock declares no props.
  let {}: Record<string, unknown> = $props();

  const DB_FLOOR = -60;
  const DB_CEIL = 0;

  // Monitoring quick-toggle cycles Off -> Monitor Only -> Monitor and Output, using
  // the same field/enum AdvAudioDialog writes via audio.setAdvanced.
  const MONITOR_CYCLE: AudioMonitoringType[] = ["none", "monitorOnly", "monitorAndOutput"];
  const MONITOR_LABEL: Record<AudioMonitoringType, string> = {
    none: "Monitoring: Off",
    monitorOnly: "Monitoring: Monitor Only",
    monitorAndOutput: "Monitoring: Monitor and Output",
  };

  function dbToPercent(db: number): number {
    if (!Number.isFinite(db) || db <= DB_FLOOR) {
      return 0;
    }
    if (db >= DB_CEIL) {
      return 100;
    }
    return ((db - DB_FLOOR) / (DB_CEIL - DB_FLOOR)) * 100;
  }

  let sources = $state<AudioSource[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);
  // Hidden rows (audio.setHidden) drop out of the list by default; the show-hidden
  // toggle reveals them (dimmed) so they can be unhidden individually. audio.list
  // already sorts pinned sources first, so the render order needs no client sort.
  let showHidden = $state(false);
  const hiddenCount = $derived(sources.filter((s) => s.hidden).length);
  const visibleSources = $derived(showHidden ? sources : sources.filter((s) => !s.hidden));
  // Mixer strip arrangement: horizontal (stacked rows) or vertical (side-by-side
  // columns), matching OBS's audio-mixer layout toggle. A frontend-only view pref
  // with no backend field, so it persists in localStorage like goLivePref (default
  // horizontal). Vertical re-flows the strips and stands each meter + fader on end
  // via CSS (.list.vertical / .row.vertical) -- a real layout change, not an icon swap.
  const LAYOUT_KEY = "obs.audioMixerVertical";
  function loadVertical(): boolean {
    try {
      return localStorage.getItem(LAYOUT_KEY) === "1";
    } catch {
      return false;
    }
  }
  let vertical = $state(loadVertical());
  function toggleLayout() {
    vertical = !vertical;
    try {
      localStorage.setItem(LAYOUT_KEY, vertical ? "1" : "0");
    } catch {
      // Non-fatal: the toggle still works for this session.
    }
  }
  let menu = $state<{ x: number; y: number; items: (ContextMenuItem | null)[] } | null>(null);
  let propsForSource = $state<string | null>(null);
  // Inline rename: keyed by source uuid (mixer rows have no scene-item id). The
  // reload on audio.changed carries the new name back into `sources`.
  let renamingUuid = $state<string | null>(null);
  let renameTo = $state("");
  // Monitoring type per source uuid. Not in audio.list, so read just-in-time from the
  // same place AdvAudioDialog reads it (audio.getAdvanced) and refreshed on each load.
  let monitoring = $state<Record<string, AudioMonitoringType>>({});

  const latest = new Map<string, { magnitude: number; peak: number }>();
  let meters = $state<Record<string, { mag: number; peak: number }>>({});
  let rafHandle = 0;

  function scheduleFlush() {
    if (rafHandle) {
      return;
    }
    rafHandle = requestAnimationFrame(() => {
      rafHandle = 0;
      const next: Record<string, { mag: number; peak: number }> = {};
      for (const [uuid, lvl] of latest) {
        next[uuid] = { mag: dbToPercent(lvl.magnitude), peak: dbToPercent(lvl.peak) };
      }
      meters = next;
    });
  }

  async function load() {
    error = null;
    try {
      const res = await obs.call("audio.list");
      sources = res.sources;
      void loadMonitoring();
      const live = new Set(sources.map((s) => s.uuid));
      for (const uuid of latest.keys()) {
        if (!live.has(uuid)) {
          latest.delete(uuid);
        }
      }
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void load();
    const offLevels = obs.on(EV.audioLevels, (p) => {
      for (const l of p.levels) {
        const cur = latest.get(l.uuid);
        if (cur) {
          cur.magnitude = l.magnitude;
          cur.peak = l.peak;
        } else {
          latest.set(l.uuid, { magnitude: l.magnitude, peak: l.peak });
        }
      }
      scheduleFlush();
    });
    const offChanged = obs.on(EV.audioChanged, () => void load());
    return () => {
      offLevels();
      offChanged();
      if (rafHandle) {
        cancelAnimationFrame(rafHandle);
        rafHandle = 0;
      }
    };
  });

  async function setDeflection(src: AudioSource, value: number) {
    src.deflection = value; // optimistic (mutating the $state element is reactive)
    try {
      const res = await obs.call("audio.setDeflection", { uuid: src.uuid, deflection: value });
      src.deflection = res.deflection;
      src.volumeDb = res.volumeDb;
    } catch (e) {
      error = (e as Error).message;
    }
  }

  function onFaderInput(src: AudioSource, e: Event) {
    void setDeflection(src, Number((e.currentTarget as HTMLInputElement).value));
  }

  async function toggleMuted(src: AudioSource) {
    const desired = !src.muted;
    src.muted = desired; // optimistic
    try {
      const res = await obs.call("audio.setMuted", { uuid: src.uuid, muted: desired });
      src.muted = res.muted;
    } catch (e) {
      error = (e as Error).message;
    }
  }

  // Mixer-state toggles. Each backend method emits audio.changed, so load() reloads
  // the list (with the pinned-first order) -- no optimistic mutation needed.
  async function setHidden(src: AudioSource, hidden: boolean) {
    try {
      await obs.call("audio.setHidden", { uuid: src.uuid, hidden });
    } catch (e) {
      error = (e as Error).message;
    }
  }

  async function unhideAll() {
    try {
      await obs.call("audio.unhideAll");
    } catch (e) {
      error = (e as Error).message;
    }
  }

  async function toggleVolumeLocked(src: AudioSource) {
    try {
      await obs.call("audio.setVolumeLocked", { uuid: src.uuid, locked: !src.volumeLocked });
    } catch (e) {
      error = (e as Error).message;
    }
  }

  async function togglePinned(src: AudioSource) {
    try {
      await obs.call("audio.setPinned", { uuid: src.uuid, pinned: !src.pinned });
    } catch (e) {
      error = (e as Error).message;
    }
  }

  // Pull each source's monitoring type in parallel; a source that can't report one
  // (rejected getAdvanced) falls back to Off so the button still renders.
  async function loadMonitoring() {
    const entries = await Promise.all(
      sources.map(async (s): Promise<[string, AudioMonitoringType]> => {
        try {
          return [s.uuid, (await obs.call("audio.getAdvanced", { uuid: s.uuid })).monitoringType];
        } catch {
          return [s.uuid, "none"];
        }
      }),
    );
    monitoring = Object.fromEntries(entries);
  }

  async function cycleMonitor(src: AudioSource) {
    const cur = monitoring[src.uuid] ?? "none";
    const next = MONITOR_CYCLE[(MONITOR_CYCLE.indexOf(cur) + 1) % MONITOR_CYCLE.length];
    monitoring = { ...monitoring, [src.uuid]: next }; // optimistic
    try {
      const aa = await obs.call("audio.setAdvanced", { uuid: src.uuid, monitoringType: next });
      monitoring = { ...monitoring, [src.uuid]: aa.monitoringType };
    } catch (e) {
      error = (e as Error).message;
    }
  }

  // Filters address the source by name (filters.* resolve via obs_get_source_by_name),
  // reusing the shared clipboard.filters chain, same as the Sources/Canvas docks.
  async function copyFilters(src: AudioSource) {
    try {
      clipboard.filters = (await obs.call("filters.copyChain", { source: src.name })).filters;
    } catch (e) {
      error = (e as Error).message;
    }
  }

  async function pasteFilters(src: AudioSource) {
    if (!clipboard.filters) {
      return;
    }
    try {
      await obs.call("filters.pasteChain", { source: src.name, filters: clipboard.filters });
    } catch (e) {
      error = (e as Error).message;
    }
  }

  function focusOnMount(node: HTMLInputElement) {
    node.focus();
    node.select();
  }

  function beginRename(src: AudioSource) {
    renamingUuid = src.uuid;
    renameTo = src.name;
  }

  // Rename the source by uuid (mixer rows carry no scene-item id). renameByName
  // applies the same clash rule as sources.rename and emits audio.changed, which
  // reloads the list with the new name.
  async function commitRename() {
    const uuid = renamingUuid;
    const name = renameTo.trim();
    renamingUuid = null;
    if (!uuid || !name) {
      return;
    }
    try {
      await obs.call("sources.renameByName", { uuid, name });
    } catch (e) {
      error = (e as Error).message;
    }
  }

  function onRenameKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      void commitRename();
    } else if (e.key === "Escape") {
      renamingUuid = null;
    }
  }

  function openMenu(e: MouseEvent, src: AudioSource) {
    e.preventDefault();
    menu = {
      x: e.clientX,
      y: e.clientY,
      items: [
        { label: "Rename", action: () => beginRename(src) },
        { label: "Properties", action: () => (propsForSource = src.name) },
        { label: "Filters", action: () => openFilters(src.name, "audio") },
        { label: "Advanced Audio Properties", action: () => openAdvAudio(src.name, src.name) },
        null,
        { label: "Hide", action: () => void setHidden(src, true) },
        { label: "Lock Volume", checked: src.volumeLocked, action: () => void toggleVolumeLocked(src) },
        { label: src.pinned ? "Unpin" : "Pin to top", action: () => void togglePinned(src) },
        null,
        { label: "Copy Filters", action: () => void copyFilters(src) },
        { label: "Paste Filters", disabled: !clipboard.filters, action: () => void pasteFilters(src) },
      ],
    };
  }

  function fmtDb(db: number): string {
    if (!Number.isFinite(db) || db <= DB_FLOOR) {
      return "-∞";
    }
    return (db > 0 ? "+" : "") + db.toFixed(1);
  }
</script>

<div class="dock-body">
  {#if error}
    <p class="dock-msg err">{error}</p>
  {/if}

  <div class="toolbar">
    {#if hiddenCount > 0}
      <button class="text-btn" title="Unhide all hidden sources" onclick={() => void unhideAll()}>
        Unhide All ({hiddenCount})
      </button>
      <button
        class="tool-btn"
        class:active={showHidden}
        title={showHidden ? "Hide hidden sources" : "Show hidden sources"}
        aria-label={showHidden ? "Hide hidden sources" : "Show hidden sources"}
        aria-pressed={showHidden}
        onclick={() => (showHidden = !showHidden)}
      >
        <Icon name={showHidden ? "eye" : "eye-off"} size={13} />
      </button>
    {/if}
    <button
      class="tool-btn layout"
      class:active={vertical}
      title={vertical ? "Switch to horizontal layout" : "Switch to vertical layout"}
      aria-label={vertical ? "Switch to horizontal layout" : "Switch to vertical layout"}
      aria-pressed={vertical}
      onclick={toggleLayout}
    >
      <Icon name={vertical ? "grid" : "list"} size={13} />
    </button>
  </div>

  {#if !loaded}
    <p class="dock-msg">Loading…</p>
  {:else if sources.length === 0}
    <p class="dock-msg">No active audio sources</p>
  {:else}
    <ul class="list" class:vertical>
      {#each visibleSources as src (src.uuid)}
        {@const m = meters[src.uuid]}
        {@const mon = monitoring[src.uuid] ?? "none"}
        <li
          class="row"
          class:vertical
          class:muted={src.muted}
          class:hidden={src.hidden}
          oncontextmenu={(e) => openMenu(e, src)}
        >
          <div class="top">
            {#if src.pinned}
              <span class="pin" title="Pinned to top" aria-hidden="true"><Icon name="star-filled" size={11} /></span>
            {/if}
            {#if renamingUuid === src.uuid}
              <input class="inline" bind:value={renameTo} onkeydown={onRenameKey} onblur={commitRename} use:focusOnMount />
            {:else}
              <span class="name" title={src.name}>{src.name}</span>
            {/if}
            <span class="db">{fmtDb(src.volumeDb)} dB</span>
          </div>
          <div class="meter" style:--lit={m ? m.mag : 0} style:--peak={m ? m.peak : 0} aria-hidden="true">
            <div class="unlit"></div>
            {#if m && m.peak > 0}
              <div class="peak"></div>
            {/if}
          </div>
          <div class="controls">
            <button
              class="tool-btn mute"
              class:on={src.muted}
              title={src.muted ? "Unmute" : "Mute"}
              aria-label={src.muted ? "Unmute" : "Mute"}
              aria-pressed={src.muted}
              onclick={() => void toggleMuted(src)}
            >
              <Icon name={src.muted ? "mute" : "volume"} size={13} />
            </button>
            <button
              class="tool-btn mon"
              class:on={mon !== "none"}
              class:both={mon === "monitorAndOutput"}
              title={MONITOR_LABEL[mon]}
              aria-label={MONITOR_LABEL[mon]}
              onclick={() => void cycleMonitor(src)}
            >
              <svg
                width="13"
                height="13"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="1.7"
                stroke-linecap="round"
                stroke-linejoin="round"
              >
                <path d="M3 18v-6a9 9 0 0 1 18 0v6" />
                <path
                  d="M21 19a2 2 0 0 1-2 2h-1a2 2 0 0 1-2-2v-3a2 2 0 0 1 2-2h3zM3 19a2 2 0 0 0 2 2h1a2 2 0 0 0 2-2v-3a2 2 0 0 0-2-2H3z"
                />
              </svg>
            </button>
            <button
              class="tool-btn lock"
              class:on={src.volumeLocked}
              title={src.volumeLocked ? "Unlock Volume" : "Lock Volume"}
              aria-label={src.volumeLocked ? "Unlock Volume" : "Lock Volume"}
              aria-pressed={src.volumeLocked}
              onclick={() => void toggleVolumeLocked(src)}
            >
              <Icon name={src.volumeLocked ? "lock" : "lock-open"} size={13} />
            </button>
            <button class="tool-btn" title="Filters" aria-label="Filters" onclick={() => openFilters(src.name, "audio")}>
              <Icon name="sliders" size={13} />
            </button>
            <button
              class="tool-btn"
              title="Advanced Audio Properties"
              aria-label="Advanced Audio Properties"
              onclick={() => openAdvAudio(src.name, src.name)}
            >
              <Icon name="gear" size={13} />
            </button>
            <input
              class="fader"
              type="range"
              min="0"
              max="1"
              step="0.01"
              value={src.deflection}
              disabled={src.volumeLocked}
              aria-label="{src.name} volume"
              oninput={(e) => onFaderInput(src, e)}
            />
          </div>
        </li>
      {/each}
    </ul>
  {/if}
</div>

{#if menu}
  <ContextMenu x={menu.x} y={menu.y} items={menu.items} onClose={() => (menu = null)} />
{/if}

{#if propsForSource}
  <PropertiesModal
    kind="source"
    ref={propsForSource}
    title={"Properties — " + propsForSource}
    onClose={() => (propsForSource = null)}
  />
{/if}

<style>
  .list {
    list-style: none;
    margin: 0;
    padding: 8px;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .row {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .top {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 8px;
  }
  .name {
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .row.muted .name {
    color: var(--color-muted);
    text-decoration: line-through;
  }
  /* Inline rename field, matching the Sources/Scenes dock rename affordance. */
  .inline {
    flex: 1;
    min-width: 0;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-accent);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 3px 5px;
  }
  .db {
    flex-shrink: 0;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
    font-variant-numeric: tabular-nums;
  }
  /* The meter track is painted full-width with the zone gradient; an .unlit cover
     masks the un-reached portion from the right. --meter-style switches the track
     between a smooth gradient and a segmented (notched) fill. */
  .meter {
    position: relative;
    height: 8px;
    overflow: hidden;
    background-color: var(--color-base);
    background-image: linear-gradient(
      90deg,
      var(--meter-green) 0%,
      var(--meter-green) 58%,
      var(--meter-yellow) 78%,
      var(--meter-red) 100%
    );
  }
  :global(:root[data-meter-style="segmented"]) .meter {
    -webkit-mask-image: repeating-linear-gradient(90deg, #000 0 4px, transparent 4px 5px);
    mask-image: repeating-linear-gradient(90deg, #000 0 4px, transparent 4px 5px);
  }
  /* Meter fill is driven by two inherited custom props set on .meter: --lit (0..100,
     the reached fraction) and --peak (0..100, the hold position). Both orientations
     read the same props so there is one source of truth for the level geometry. */
  .unlit {
    position: absolute;
    top: 0;
    right: 0;
    bottom: 0;
    width: calc((100 - var(--lit, 0)) * 1%);
    background: var(--color-base);
  }
  .peak {
    position: absolute;
    top: 0;
    bottom: 0;
    left: calc(var(--peak, 0) * 1%);
    width: 2px;
    background: var(--color-text);
  }
  .controls {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .mute.on {
    color: var(--color-live);
  }
  /* Monitoring quick-toggle: dim when Off, accent for Monitor Only, live tint when
     also routed to output. Order matters -- .both must win over .on. */
  .mon {
    color: var(--color-muted);
  }
  .mon.on {
    color: var(--color-accent);
  }
  .mon.both {
    color: var(--color-live);
  }
  /* Volume-lock toggle: accent tint when locked (the fader is disabled alongside). */
  .lock.on {
    color: var(--color-accent);
  }
  /* Hide/Unhide header: text action + show-hidden eye toggle. */
  .toolbar {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 6px 8px 0;
  }
  .text-btn {
    flex: 1;
    min-width: 0;
    text-align: left;
    background: none;
    border: 0;
    padding: 0;
    cursor: pointer;
    font-family: var(--font-ui);
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-muted);
  }
  .text-btn:hover {
    color: var(--color-accent);
  }
  /* Pinned marker before the source name. */
  .pin {
    flex-shrink: 0;
    display: inline-flex;
    color: var(--color-accent);
  }
  /* Hidden rows only render with the show-hidden toggle on; dim them so they read as hidden. */
  .row.hidden {
    opacity: 0.5;
  }
  .fader {
    flex: 1;
    min-width: 0;
    accent-color: var(--color-accent);
  }
  /* Audio mixer messages use a roomier pad than the shared 8px 7px default. */
  .dock-msg {
    padding: 10px 9px;
  }

  /* Layout toggle always hugs the trailing edge of the toolbar (whether or not the
     Unhide-All/show-hidden controls are present). */
  .toolbar .layout {
    margin-left: auto;
  }

  /* Vertical layout (OBS "Vertical Layout"): strips flow left-to-right and wrap into
     columns instead of stacking, and each strip stands its meter + fader on end. */
  .list.vertical {
    flex-direction: row;
    flex-wrap: wrap;
    align-items: flex-start;
    gap: 8px;
  }
  .row.vertical {
    width: 96px;
    padding: 8px 6px;
    gap: 8px;
    border: var(--border-weight) solid var(--color-border);
  }
  .row.vertical .top {
    justify-content: center;
    gap: 6px;
  }
  .row.vertical .name {
    min-width: 0;
  }
  /* Meter stands vertically: gradient runs bottom(green)->top(red), the unlit cover
     masks from the top, and the peak hold becomes a horizontal tick. */
  .row.vertical .meter {
    width: 8px;
    height: 120px;
    align-self: center;
    background-image: linear-gradient(
      0deg,
      var(--meter-green) 0%,
      var(--meter-green) 58%,
      var(--meter-yellow) 78%,
      var(--meter-red) 100%
    );
  }
  .row.vertical .unlit {
    top: 0;
    right: 0;
    left: 0;
    bottom: auto;
    width: 100%;
    height: calc((100 - var(--lit, 0)) * 1%);
  }
  .row.vertical .peak {
    top: auto;
    right: 0;
    left: 0;
    bottom: calc(var(--peak, 0) * 1%);
    width: 100%;
    height: 2px;
  }
  :global(:root[data-meter-style="segmented"]) .row.vertical .meter {
    -webkit-mask-image: repeating-linear-gradient(0deg, #000 0 4px, transparent 4px 5px);
    mask-image: repeating-linear-gradient(0deg, #000 0 4px, transparent 4px 5px);
  }
  /* Controls become a 2-col grid; the fader stands on end spanning the top row, with
     the button cluster flowing beneath it. */
  .row.vertical .controls {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    align-items: center;
    justify-items: center;
    gap: 6px 4px;
  }
  .row.vertical .fader {
    order: -1;
    grid-column: 1 / -1;
    justify-self: center;
    width: 24px;
    height: 120px;
    writing-mode: vertical-lr;
    direction: rtl;
    appearance: slider-vertical;
    -webkit-appearance: slider-vertical;
  }
</style>
