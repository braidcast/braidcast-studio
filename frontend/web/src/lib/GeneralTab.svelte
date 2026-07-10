<script lang="ts">
  import { obs, type GeneralSettings } from "./bridge";
import { EV } from "./eventNames";
  import { openMissingFiles } from "./missingFilesOpener.svelte";
  import { openLogViewer } from "./logViewerOpener.svelte";
  import { openImporter } from "./importerOpener.svelte";
  import { goLivePref, setAskStreamInfo } from "./goLivePrefStore.svelte";
  import ToggleSwitch from "./ToggleSwitch.svelte";

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

  let s = $state<GeneralSettings>({
    projectorAlwaysOnTop: false,
    snapEnabled: true,
    snapDistance: 10,
    snapToEdge: true,
    snapToSource: true,
    snapToCenter: true,
    warnBeforeGoLive: false,
    warnBeforeStop: false,
    startMinimized: false,
    minimizeToTray: false,
    alwaysShowTray: false,
    multiviewLayout: "horizontalTop",
    multiviewDrawNames: true,
    multiviewDrawSafeAreas: false,
    importerPrompts: true,
    scenesGridMode: false,
  });

  let loaded = $state(false);
  let error = $state<string | null>(null);

  $effect(() => {
    let active = true;
    obs
      .call("settings.getGeneral")
      .then((g) => {
        if (active) s = g;
      })
      .catch((e) => {
        if (active) error = (e as Error).message;
      })
      .finally(() => {
        if (active) loaded = true;
      });
    const off = obs.on(EV.settingsGeneralChanged, (g) => (s = g));
    return () => {
      active = false;
      off();
    };
  });

  // Optimistic local set, then reconcile from the echoed full state.
  async function apply(patch: Partial<GeneralSettings>): Promise<void> {
    error = null;
    s = { ...s, ...patch };
    try {
      s = await obs.call("settings.setGeneral", patch);
    } catch (e) {
      error = (e as Error).message;
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
      <ToggleSwitch size="sm" checked={goLivePref.askStreamInfo} onchange={(v) => setAskStreamInfo(v)} />
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
    <button class="action" onclick={() => openImporter()}>Import from OBS Studio…</button>
    <p class="dim note">Bring scene collections, stream destinations, and video/audio settings in from an OBS Studio install.</p>
  </section>

  <section class="group">
    <h4>Sources</h4>
    <button class="action" onclick={() => openMissingFiles()}>Find Missing Files…</button>
    <p class="dim note">Locate and relink sources whose media file has moved or been renamed.</p>
  </section>

  <section class="group">
    <h4>Diagnostics</h4>
    <button class="action" onclick={() => openLogViewer()}>View Current Log</button>
    <p class="dim note">Show the current session log for troubleshooting and bug reports.</p>
  </section>

  {#if error}<p class="error">{error}</p>{/if}
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
  .action {
    height: auto;
    padding: 7px 12px;
    font-family: var(--font-ui);
    font-size: 12px;
    border: var(--border-weight) solid var(--color-border);
    background: transparent;
    color: var(--color-text);
    cursor: pointer;
  }
  .action:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .hint {
    font-size: 12px;
    margin-top: 8px;
  }
  .note {
    font-size: 12px;
    margin-top: 8px;
  }
  .error {
    color: var(--color-live);
    margin: 6px 0 0;
    font-size: 12px;
  }
</style>
