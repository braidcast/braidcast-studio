<script lang="ts">
  import { obs, type McpConfig, type McpSetConfigParams } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import PageHeader from "$lib/ui/PageHeader.svelte";

  // MCP control page. Logic mirrors McpTab.svelte (load + mcp.changed subscription,
  // optimistic apply, copy-to-clipboard, token mask, regenerate confirm) laid out to
  // the redesign mock; kept self-contained so the restore-chip McpTab is untouched.
  let cfg = $state<McpConfig | null>(null);
  let loaded = $state(false);
  let busy = $state(false);

  let showToken = $state(false);
  let copied = $state<"" | "endpoint" | "token">("");
  let copyTimer: ReturnType<typeof setTimeout> | undefined;

  const clipboardAvailable =
    typeof navigator !== "undefined" && !!navigator.clipboard && typeof navigator.clipboard.writeText === "function";

  function load(): void {
    obs
      .call("mcp.getConfig")
      .then((next) => (cfg = next))
      .catch(() => {})
      .finally(() => (loaded = true));
  }

  $effect(() => {
    load();
    const off = obs.on(EV.mcpChanged, () => load());
    return () => {
      off();
      if (copyTimer) {
        clearTimeout(copyTimer);
      }
    };
  });

  // Optimistically patch, then reconcile from the server echo; revert by re-pulling.
  async function apply(patch: McpSetConfigParams): Promise<void> {
    if (!cfg || busy) {
      return;
    }
    busy = true;
    cfg = { ...cfg, ...patch };
    try {
      cfg = await obs.call("mcp.setConfig", patch);
    } catch {
      load();
    } finally {
      busy = false;
    }
  }

  async function regenerate(): Promise<void> {
    if (!cfg || busy) {
      return;
    }
    if (
      !confirm(
        "Regenerate the token? Any MCP client using the current token will stop working until you paste the new one.",
      )
    ) {
      return;
    }
    busy = true;
    try {
      const res = await obs.call("mcp.regenerateToken");
      cfg = { ...cfg, token: res.token };
      showToken = true;
    } catch {
      load();
    } finally {
      busy = false;
    }
  }

  async function copy(which: "endpoint" | "token", value: string): Promise<void> {
    if (!clipboardAvailable) {
      return;
    }
    try {
      await navigator.clipboard.writeText(value);
      copied = which;
      if (copyTimer) {
        clearTimeout(copyTimer);
      }
      copyTimer = setTimeout(() => (copied = ""), 1400);
    } catch {
      // Copy is best-effort; ignore failures.
    }
  }

  const maskedToken = $derived(cfg ? (showToken ? cfg.token : "•".repeat(Math.min(cfg.token.length, 32))) : "");
</script>

<div class="page">
  <PageHeader title="AI Control" sub="drive the production over MCP">
    {#snippet actions()}
      {#if cfg}
        <button
          class="server-toggle"
          class:on={cfg.enabled}
          disabled={busy}
          onclick={() => void apply({ enabled: !cfg!.enabled })}
        >
          <span class="srv-dot" style:background={cfg.enabled ? "var(--meter-green)" : "var(--color-muted)"}></span>
          {cfg.enabled ? "Server Enabled" : "Server Disabled"}
        </button>
      {/if}
    {/snippet}
  </PageHeader>

  <div class="body">
    {#if !loaded}
      <p class="msg">Loading…</p>
    {:else if !cfg}
      <p class="msg">AI control unavailable.</p>
    {:else}
      <div class="cols">
        <div class="col-left">
          <section class="card" class:disabled={!cfg.enabled}>
            <h3 class="card-title">CONNECTION</h3>

            <div class="sub-block">
              <span class="field-label">Endpoint</span>
              <div class="inline">
                <code class="code grow">{cfg.endpoint}</code>
                {#if clipboardAvailable}
                  <button class="copy-btn" onclick={() => void copy("endpoint", cfg!.endpoint)}>
                    {copied === "endpoint" ? "Copied" : "Copy"}
                  </button>
                {/if}
              </div>
            </div>

            <div class="sub-block">
              <span class="field-label">Auth token</span>
              <div class="inline">
                <code class="code grow">{maskedToken}</code>
                <button class="copy-btn" onclick={() => (showToken = !showToken)}>
                  {showToken ? "Hide" : "Show"}
                </button>
                {#if clipboardAvailable}
                  <button class="copy-btn" onclick={() => void copy("token", cfg!.token)}>
                    {copied === "token" ? "Copied" : "Copy"}
                  </button>
                {/if}
                <button class="copy-btn" disabled={busy} onclick={() => void regenerate()}>Regenerate</button>
              </div>
            </div>
          </section>

          <section class="card" class:disabled={!cfg.enabled}>
            <h3 class="card-title">CAPABILITIES</h3>

            <div class="cap-row">
              <div class="cap-text">
                <span class="cap-name">Read state</span>
                <span class="cap-desc">list scenes, sources, canvases, stats — always available</span>
              </div>
              <button class="sw on locked" disabled aria-label="Read state (always on)">
                <span class="knob"></span>
              </button>
            </div>

            <div class="cap-row">
              <div class="cap-text">
                <span class="cap-name">Mutate</span>
                <span class="cap-desc">switch scenes, toggle sources, edit transforms</span>
              </div>
              <button
                class="sw"
                class:on={cfg.allowMutations}
                disabled={!cfg.enabled}
                aria-pressed={cfg.allowMutations}
                aria-label="Allow mutations"
                onclick={() => void apply({ allowMutations: !cfg!.allowMutations })}
              >
                <span class="knob"></span>
              </button>
            </div>

            <div class="cap-row last">
              <div class="cap-text">
                <span class="cap-name">Go-Live control</span>
                <span class="cap-desc">start / stop outputs — outward action, off by default</span>
              </div>
              <button
                class="sw"
                class:on={cfg.allowGoLive}
                disabled={!cfg.enabled}
                aria-pressed={cfg.allowGoLive}
                aria-label="Allow go-live"
                onclick={() => void apply({ allowGoLive: !cfg!.allowGoLive })}
              >
                <span class="knob"></span>
              </button>
            </div>
          </section>
        </div>

        <section class="col-right">
          <div class="right-head">RECENT TOOL CALLS</div>
          <div class="right-body">
            <div class="right-empty">
              <p>No tool calls recorded</p>
              <p class="right-empty-sub">Tool-call history isn't captured yet</p>
            </div>
          </div>
        </section>
      </div>
    {/if}
  </div>
</div>

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    background: var(--color-base);
    color: var(--color-text);
  }
  .server-toggle {
    display: flex;
    align-items: center;
    gap: 8px;
    height: auto;
    padding: 8px 16px;
    font-family: var(--font-ui);
    font-size: 12px;
    font-weight: 600;
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
  }
  .server-toggle.on {
    border-color: var(--meter-green);
    background: color-mix(in srgb, var(--meter-green) 12%, transparent);
    color: var(--meter-green);
  }
  .server-toggle:hover {
    border-color: var(--color-border);
  }
  .server-toggle.on:hover {
    border-color: var(--meter-green);
  }
  .srv-dot {
    width: 8px;
    height: 8px;
  }

  .body {
    flex: 1;
    min-height: 0;
    overflow: auto;
    padding: 22px 24px;
  }
  .msg {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 12px;
    color: var(--color-muted);
  }

  .cols {
    display: flex;
    gap: 18px;
    align-items: flex-start;
  }
  .col-left {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 18px;
    min-width: 0;
  }
  .card {
    padding: 18px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .card.disabled {
    opacity: 0.5;
    pointer-events: none;
  }
  .card-title {
    margin: 0 0 14px;
    font-family: var(--font-mono);
    font-size: 10px;
    font-weight: 400;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
  }

  .sub-block {
    margin-bottom: 16px;
  }
  .sub-block:last-child {
    margin-bottom: 0;
  }
  .field-label {
    display: block;
    margin-bottom: 6px;
    font-size: 11px;
    color: var(--color-dim);
  }
  .inline {
    display: flex;
    align-items: center;
    gap: 6px;
    flex-wrap: wrap;
  }
  .code {
    font-family: var(--font-mono);
    font-size: 12px;
    color: var(--color-text);
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    padding: 9px 12px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .code.grow {
    flex: 1 1 220px;
    min-width: 0;
  }
  .copy-btn {
    flex: 0 0 auto;
    height: auto;
    padding: 7px 13px;
    font-family: var(--font-ui);
    font-size: 11px;
    background: none;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
  }
  .copy-btn:hover:not(:disabled) {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }

  .cap-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 16px;
    padding: 12px 0;
    border-bottom: var(--border-weight) solid var(--color-border-2);
  }
  .cap-row.last {
    border-bottom: none;
  }
  .cap-text {
    display: flex;
    flex-direction: column;
    gap: 3px;
    min-width: 0;
  }
  .cap-name {
    font-size: 13px;
    color: var(--color-text);
  }
  .cap-desc {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .sw {
    position: relative;
    flex: 0 0 auto;
    width: 38px;
    height: 20px;
    padding: 0;
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
  }
  .sw.on {
    border-color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 26%, transparent);
  }
  .sw.locked {
    cursor: default;
    opacity: 0.6;
  }
  .knob {
    position: absolute;
    top: 50%;
    left: 2px;
    transform: translateY(-50%);
    width: 14px;
    height: 14px;
    background: var(--color-muted);
  }
  .sw.on .knob {
    left: 20px;
    background: var(--color-accent);
  }

  .col-right {
    flex: 0 0 380px;
    align-self: stretch;
    display: flex;
    flex-direction: column;
    min-height: 0;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .right-head {
    flex: 0 0 auto;
    padding: 14px 16px;
    border-bottom: var(--border-weight) solid var(--color-border);
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .right-body {
    flex: 1;
    min-height: 0;
    overflow: auto;
    display: flex;
    align-items: center;
    justify-content: center;
  }
  .right-empty {
    text-align: center;
    font-family: var(--font-mono);
    color: var(--color-muted);
  }
  .right-empty p {
    margin: 0;
    font-size: 12px;
  }
  .right-empty-sub {
    margin-top: 6px !important;
    font-size: 10px;
    color: var(--color-muted);
    opacity: 0.7;
  }
</style>
