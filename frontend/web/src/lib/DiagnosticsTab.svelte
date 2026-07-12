<script lang="ts">
  import { obs } from "./bridge";
  import { diagnosticsStore } from "./diagnosticsStore.svelte";
  import ToggleSwitch from "./ToggleSwitch.svelte";

  // Diagnostics settings, live-applied. The DEBUG gate + session-log path are owned
  // by the shared diagnosticsStore (seeded from diagnostics.get, kept live by the
  // debug.changed event); this tab only issues setDebug / openLogFolder and reads the
  // store reactively. Start is idempotent (App.svelte already starts it at boot).
  $effect(() => {
    diagnosticsStore.start();
  });

  let error = $state<string | null>(null);

  // Optimistic flip for responsiveness; the debug.changed echo reconciles the store.
  async function setDebug(enabled: boolean): Promise<void> {
    error = null;
    diagnosticsStore.debug = enabled;
    try {
      await obs.call("diagnostics.setDebug", { enabled });
    } catch (e) {
      error = (e as Error).message;
    }
  }

  async function openLogFolder(): Promise<void> {
    error = null;
    try {
      await obs.call("diagnostics.openLogFolder");
    } catch (e) {
      error = (e as Error).message;
    }
  }
</script>

<section class="group">
  <h4>Debug Logging</h4>
  <label class="check">
    <ToggleSwitch size="sm" checked={diagnosticsStore.debug} onchange={(v) => void setDebug(v)} />
    Enable debug logging
  </label>
  <p class="dim note">
    Writes verbose, category-tagged diagnostics to the session log. Leave off for normal use; turn on to reproduce a
    problem, then attach the log to a bug report.
  </p>
</section>

<section class="group">
  <h4>Log Files</h4>
  <button class="action" onclick={() => void openLogFolder()}>Open Log Folder</button>
  <p class="dim note">Reveal the folder containing the session logs in your file manager.</p>
  {#if diagnosticsStore.logPath}
    <p class="dim path">{diagnosticsStore.logPath}</p>
  {/if}
</section>

{#if error}<p class="error">{error}</p>{/if}

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
  .check {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 8px;
    font-size: 13px;
    color: var(--color-text);
    cursor: pointer;
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
  .note {
    font-size: 12px;
    margin-top: 8px;
  }
  .path {
    font-family: var(--font-mono, monospace);
    font-size: 11px;
    margin-top: 8px;
    word-break: break-all;
  }
  .error {
    color: var(--color-live);
    margin: 6px 0 0;
    font-size: 12px;
  }
</style>
