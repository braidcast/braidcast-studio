<script lang="ts">
  import { obs } from "$lib/api/bridge";
  import { diagnosticsStore } from "$lib/stores/diagnosticsStore.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";
  import { DEVTOOLS_EXPOSURE, DEVTOOLS_OPT_IN } from "$lib/utils/devToolsExposure";

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

<!-- Detail view for the title-bar badge, which stays the primary sign. All three states are
     drawn, because 0 is not by itself evidence of a closed port: it is also the value before
     the seeding diagnostics.get resolves and the value it keeps forever if that call rejects.
     Only a settled, error-free read may say "closed" out loud; anything else says unknown.
     DebugPortBadge draws nothing during the transient pre-seed window and its own muted
     "?" once a read has settled badly -- silence there is a deliberate bet on a one
     round-trip delay, not a way of saying unknown, which is why this panel and that badge
     both need an explicit unknown state rather than an absence.
     The closed row is the not-applicable reading rather than a milder warning -- it takes
     the muted token STATE_COLOR.idle/unavailable use for "this is fine, it just isn't
     part of what's running", so it cannot be read as a downgraded alert. -->
<section class="group">
  <h4>Remote Debugging</h4>
  {#if diagnosticsStore.devToolsPort > 0}
    <p class="port open"><Icon name="warn" size={12} />Port {diagnosticsStore.devToolsPort} is open</p>
    <p class="dim note">{DEVTOOLS_EXPOSURE}</p>
    <p class="dim note">
      It was opened by <code>{DEVTOOLS_OPT_IN}</code> in this machine's environment. Clearing either variable closes
      it. The port is fixed when Braidcast starts, so the change takes effect on the next launch.
    </p>
  {:else if diagnosticsStore.loaded && !diagnosticsStore.error}
    <p class="port closed"><Icon name="dot" size={10} />No debugging port is open</p>
    <p class="dim note">
      Braidcast listens for a remote debugger only when <code>{DEVTOOLS_OPT_IN}</code> is set in this machine's
      environment. The port is decided when Braidcast starts and cannot be opened while it is running.
    </p>
  {:else}
    <p class="port unknown"><span class="mark" aria-hidden="true">?</span>Debug port status unknown</p>
    <p class="dim note">
      Braidcast has not been able to read its diagnostics state, so this cannot say whether a debugging port is open.
      Treat it as unknown rather than closed. The title-bar warning appears whenever a port is known to be open.
    </p>
  {/if}
</section>

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
  .port {
    display: flex;
    align-items: center;
    gap: 6px;
    margin: 0;
    font-family: var(--font-mono, monospace);
    font-size: 12px;
  }
  /* Filled chip, for the same reason the title-bar badge is one: --color-warn as text
     lands at 1.84:1 on the light mode's --color-surface and no mode can rewrite it,
     while ink on the fill is 10.03:1 in both. inline-flex so the chip wraps the label
     instead of painting a full-width bar. */
  .port.open {
    display: inline-flex;
    padding: 3px 7px;
    color: var(--color-warn-ink);
    background: var(--color-warn);
    border: var(--border-weight) solid var(--color-warn-ink);
  }
  .port.closed {
    color: var(--color-muted);
  }
  /* Unknown sits above closed, not beside it: withholding a claim has to be visibly
     different from making one, so it takes the brighter neutral and the "?" the badge
     uses for the same state rather than reusing the closed row's dot. */
  .port.unknown {
    color: var(--color-dim);
  }
  .mark {
    width: 10px;
    text-align: center;
    font-weight: 600;
  }
  /* Both open-state notes carry the warn strip -- the exposure sentence and the how-to-
     close sentence are one block. General sibling, not adjacent: the adjacent form
     reached only the first of the two. The closed and unknown rows carry no accent, so
     none of the three ever reads as an intensity of the same alert. */
  .port.open ~ .note {
    border-left: 2px solid var(--color-warn);
    padding-left: 9px;
  }
  code {
    font-family: var(--font-mono, monospace);
    font-size: 11px;
    color: var(--color-dim);
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
