<script lang="ts">
  import { obs, type McpConfig, type McpSetConfigParams } from "./bridge";

  let cfg = $state<McpConfig | null>(null);
  let loaded = $state(false);
  let error = $state<string | null>(null);
  let busy = $state(false);

  // Token masking + transient "Copied" affordances (keyed by which field copied).
  let showToken = $state(false);
  let copied = $state<"" | "endpoint" | "token">("");
  let copyTimer: ReturnType<typeof setTimeout> | undefined;

  // Local edit buffer for the port field so typing a partial value doesn't fight
  // the reconciled config; committed on change/Enter.
  let portInput = $state(0);

  const clipboardAvailable =
    typeof navigator !== "undefined" && !!navigator.clipboard && typeof navigator.clipboard.writeText === "function";

  async function load() {
    error = null;
    try {
      const next = await obs.call("mcp.getConfig");
      reconcile(next);
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  function reconcile(next: McpConfig) {
    cfg = next;
    portInput = next.port;
  }

  $effect(() => {
    void load();
    const off = obs.on("mcp.changed", () => void load());
    return () => {
      off();
      if (copyTimer) clearTimeout(copyTimer);
    };
  });

  // Optimistically patch the local config, then reconcile from the server's echo.
  async function apply(patch: McpSetConfigParams) {
    if (!cfg || busy) return;
    busy = true;
    error = null;
    const optimistic = { ...cfg, ...patch };
    cfg = optimistic;
    try {
      const next = await obs.call("mcp.setConfig", patch);
      reconcile(next);
    } catch (e) {
      error = (e as Error).message;
      // Revert the optimistic guess by re-pulling the authoritative config.
      await load();
    } finally {
      busy = false;
    }
  }

  function commitPort() {
    if (!cfg) return;
    const p = Math.trunc(portInput);
    if (!Number.isInteger(p) || p < 1024 || p > 65535) {
      error = "Port must be between 1024 and 65535.";
      portInput = cfg.port;
      return;
    }
    if (p === cfg.port) return;
    void apply({ port: p });
  }

  async function regenerate() {
    if (!cfg || busy) return;
    if (!confirm("Regenerate the token? Any MCP client using the current token will stop working until you paste the new one.")) {
      return;
    }
    busy = true;
    error = null;
    try {
      const res = await obs.call("mcp.regenerateToken");
      cfg = { ...cfg, token: res.token };
      showToken = true;
    } catch (e) {
      error = (e as Error).message;
    } finally {
      busy = false;
    }
  }

  async function copy(which: "endpoint" | "token", value: string) {
    if (!clipboardAvailable) return;
    try {
      await navigator.clipboard.writeText(value);
      copied = which;
      if (copyTimer) clearTimeout(copyTimer);
      copyTimer = setTimeout(() => (copied = ""), 1400);
    } catch (e) {
      error = "Copy failed: " + (e as Error).message;
    }
  }

  const maskedToken = $derived(cfg ? (showToken ? cfg.token : "•".repeat(Math.min(cfg.token.length, 32))) : "");
</script>

<div class="mcp">
  {#if error}<p class="error">{error}</p>{/if}

  {#if !loaded}
    <p class="dim">Loading AI control settings…</p>
  {:else if !cfg}
    <p class="dim">AI control unavailable.</p>
  {:else}
    <section class="group">
      <h4>AI Control (MCP)</h4>

      <div class="row">
        <div class="row-main">
          <span class="flabel">Enable MCP server</span>
          <span class="help">Let an AI client connect to and drive this app over a local endpoint.</span>
        </div>
        <label class="toggle" title={cfg.enabled ? "Enabled" : "Disabled"}>
          <input
            type="checkbox"
            checked={cfg.enabled}
            disabled={busy}
            onchange={(e) => void apply({ enabled: (e.currentTarget as HTMLInputElement).checked })}
          />
          <span class="track"><span class="thumb"></span></span>
        </label>
      </div>

      <div class="status">
        {#if !cfg.enabled}
          <span class="dot off"></span>
          <span class="status-text dim">server stopped</span>
        {:else if cfg.listening}
          <span class="dot on"></span>
          <span class="status-text on">listening on :{cfg.port}</span>
        {:else}
          <span class="dot err"></span>
          <span class="status-text err">{cfg.lastError || "not listening"}</span>
        {/if}
      </div>
    </section>

    <div class="body" class:disabled={!cfg.enabled} aria-disabled={!cfg.enabled}>
      <section class="group">
        <h4>Connection</h4>

        <div class="field">
          <span class="flabel">Port</span>
          <input
            type="number"
            min="1024"
            max="65535"
            bind:value={portInput}
            disabled={!cfg.enabled || busy}
            onchange={commitPort}
            onkeydown={(e) => {
              if (e.key === "Enter") commitPort();
            }}
            aria-label="MCP server port"
          />
        </div>

        <div class="field">
          <span class="flabel">Endpoint URL</span>
          <div class="inline">
            <input class="grow mono" type="text" readonly value={cfg.endpoint} aria-label="MCP endpoint URL" />
            {#if clipboardAvailable}
              <button class="mini" disabled={!cfg.enabled} onclick={() => void copy("endpoint", cfg!.endpoint)}>
                {copied === "endpoint" ? "Copied" : "Copy"}
              </button>
            {/if}
          </div>
          <span class="help">Paste this into your MCP client as the server URL.</span>
        </div>

        <div class="field">
          <span class="flabel">Token</span>
          <div class="inline">
            <input
              class="grow mono"
              type="text"
              readonly
              value={maskedToken}
              aria-label="MCP access token"
            />
            <button class="mini" disabled={!cfg.enabled} onclick={() => (showToken = !showToken)}>
              {showToken ? "Hide" : "Show"}
            </button>
            {#if clipboardAvailable}
              <button class="mini" disabled={!cfg.enabled} onclick={() => void copy("token", cfg!.token)}>
                {copied === "token" ? "Copied" : "Copy"}
              </button>
            {/if}
            <button class="mini danger" disabled={!cfg.enabled || busy} onclick={() => void regenerate()}>
              Regenerate
            </button>
          </div>
          <span class="help">A bearer token authenticating the client. Regenerating invalidates existing clients.</span>
        </div>
      </section>

      <section class="group">
        <h4>Permissions</h4>

        <div class="row">
          <div class="row-main">
            <span class="flabel">Allow mutations</span>
            <span class="help">Let the AI create/modify/delete sources &amp; scenes (not just read).</span>
          </div>
          <label class="toggle" title={cfg.allowMutations ? "Allowed" : "Read-only"}>
            <input
              type="checkbox"
              checked={cfg.allowMutations}
              disabled={!cfg.enabled || busy}
              onchange={(e) => void apply({ allowMutations: (e.currentTarget as HTMLInputElement).checked })}
            />
            <span class="track"><span class="thumb"></span></span>
          </label>
        </div>

        <div class="row">
          <div class="row-main">
            <span class="flabel warn">Allow go-live</span>
            <span class="help">Let the AI start/stop streaming. Off by default.</span>
          </div>
          <label class="toggle warn" title={cfg.allowGoLive ? "Allowed" : "Blocked"}>
            <input
              type="checkbox"
              checked={cfg.allowGoLive}
              disabled={!cfg.enabled || busy}
              onchange={(e) => void apply({ allowGoLive: (e.currentTarget as HTMLInputElement).checked })}
            />
            <span class="track"><span class="thumb"></span></span>
          </label>
        </div>
      </section>

      <section class="group hintblock">
        <h4>Connecting a client</h4>
        <p class="hint">Add an MCP server in your AI client using the Endpoint URL above.</p>
        <p class="hint">Authenticate with the Token as a bearer credential.</p>
      </section>
    </div>
  {/if}
</div>

<style>
  .mcp {
    padding: 8px 0 4px;
  }
  .group {
    padding: 4px 0 14px;
    border-bottom: 1px solid var(--border);
  }
  .group:last-child {
    border-bottom: none;
  }
  .group h4 {
    margin: 0 0 10px;
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--text-dim);
  }
  .body.disabled {
    opacity: 0.5;
    pointer-events: none;
  }
  .row {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
    gap: 12px;
    margin-bottom: 12px;
  }
  .row-main {
    min-width: 0;
  }
  .field {
    margin-bottom: 14px;
  }
  .flabel {
    display: block;
    font-size: 12px;
    color: var(--text-soft);
    margin-bottom: 6px;
  }
  .flabel.warn {
    color: var(--meter-yellow, #eab308);
  }
  .help {
    display: block;
    font-size: 11px;
    color: var(--text-dim);
    line-height: 1.35;
  }
  .status {
    display: flex;
    align-items: center;
    gap: 7px;
  }
  .dot {
    width: 8px;
    height: 8px;
    border-radius: 0;
    flex-shrink: 0;
  }
  .dot.on {
    background: var(--meter-green, #3fae4a);
  }
  .dot.err {
    background: var(--off, #d65a5a);
  }
  .dot.off {
    background: var(--text-dim);
  }
  .status-text {
    font-size: 12px;
  }
  .status-text.on {
    color: var(--meter-green, #3fae4a);
  }
  .status-text.err {
    color: var(--off, #d65a5a);
  }
  .inline {
    display: flex;
    align-items: center;
    gap: 6px;
    flex-wrap: wrap;
  }
  input {
    background: var(--bg-sunken);
    border: 1px solid var(--border);
    border-radius: 0;
    padding: 7px 10px;
    color: var(--text);
    font: inherit;
    width: 120px;
  }
  input:focus {
    outline: none;
    border-color: var(--accent);
  }
  input[readonly] {
    color: var(--text-soft);
  }
  input.grow {
    flex: 1 1 220px;
    width: auto;
    min-width: 0;
  }
  input.mono {
    font-family: var(--font-mono);
    font-size: 12px;
  }
  .mini {
    background: var(--bg-raised);
    border: 1px solid var(--border);
    border-radius: 0;
    color: var(--text-soft);
    cursor: pointer;
    font: inherit;
    font-size: 12px;
    padding: 6px 10px;
    line-height: 1;
    flex-shrink: 0;
  }
  .mini:hover:not(:disabled) {
    color: var(--text);
    border-color: var(--accent);
  }
  .mini:disabled {
    opacity: 0.4;
    cursor: default;
  }
  .mini.danger:hover:not(:disabled) {
    color: var(--off, #d65a5a);
    border-color: var(--off, #d65a5a);
  }
  .toggle {
    display: inline-flex;
    align-items: center;
    cursor: pointer;
    flex-shrink: 0;
  }
  .toggle input {
    position: absolute;
    opacity: 0;
    width: 0;
    height: 0;
  }
  .track {
    display: inline-block;
    width: 34px;
    height: 18px;
    border-radius: 999px;
    background: var(--bg-raised);
    border: 1px solid var(--border);
    position: relative;
    transition: background 0.12s ease;
  }
  .thumb {
    position: absolute;
    top: 1px;
    left: 1px;
    width: 14px;
    height: 14px;
    border-radius: 50%;
    background: var(--text-dim);
    transition:
      transform 0.12s ease,
      background 0.12s ease;
  }
  .toggle input:checked + .track {
    background: var(--accent);
    border-color: var(--accent);
  }
  .toggle.warn input:checked + .track {
    background: var(--meter-yellow, #eab308);
    border-color: var(--meter-yellow, #eab308);
  }
  .toggle input:checked + .track .thumb {
    transform: translateX(16px);
    background: #fff;
  }
  .toggle input:disabled + .track {
    opacity: 0.5;
    cursor: default;
  }
  .hintblock .hint {
    margin: 0 0 4px;
    font-size: 12px;
    color: var(--text-dim);
    line-height: 1.4;
  }
  .dim {
    color: var(--text-dim);
    margin: 0;
  }
  .error {
    color: var(--off, #d65a5a);
    margin: 0 0 8px;
    font-size: 12px;
  }
</style>
