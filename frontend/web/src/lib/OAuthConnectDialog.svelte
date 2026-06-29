<script lang="ts">
  import { obs, type DeviceCodePrompt } from "./bridge";
  import { markOAuthConnected, type OAuthConnectRequest } from "./oauthConnectOpener.svelte";

  interface Props {
    req: OAuthConnectRequest;
    onClose: () => void;
  }
  let { req, onClose }: Props = $props();

  // The native preview overlay is suspended by the opener for this dialog's whole
  // lifetime, so it never occludes the modal.

  type Phase = "starting" | "waiting" | "error";
  let phase = $state<Phase>("starting");
  let prompt = $state<DeviceCodePrompt | null>(null);
  let errorMsg = $state<string | null>(null);
  let remaining = $state(0);
  let timer: ReturnType<typeof setInterval> | null = null;

  function stopTimer() {
    if (timer !== null) {
      clearInterval(timer);
      timer = null;
    }
  }

  function startCountdown(sec: number) {
    stopTimer();
    remaining = sec;
    if (sec <= 0) {
      return;
    }
    timer = setInterval(() => {
      remaining -= 1;
      if (remaining <= 0) {
        stopTimer();
      }
    }, 1000);
  }

  function fmt(sec: number): string {
    const m = Math.floor(sec / 60);
    const s = sec % 60;
    return m + ":" + (s < 10 ? "0" : "") + s;
  }

  // Kick off (or retry) the device-code flow. The call returns immediately
  // ({pending:true}); the user code + verification URL arrive via oauth.deviceCode.
  async function begin() {
    phase = "starting";
    errorMsg = null;
    prompt = null;
    stopTimer();
    try {
      await obs.call("oauth.connect", { providerId: req.providerId, profileUuid: req.profileUuid });
    } catch (e) {
      phase = "error";
      errorMsg = (e as Error).message;
    }
  }

  async function checkConnected() {
    try {
      const statuses = await obs.call("oauth.status");
      const me = statuses.find((s) => s.profileUuid === req.profileUuid);
      if (me && me.connected) {
        // Linked: the poll already finished, so the close below must not cancel it.
        markOAuthConnected();
        onClose();
      }
    } catch {
      // Ignore: the Streams tab's own oauth.status subscription also refreshes.
    }
  }

  $effect(() => {
    void begin();
    const offCode = obs.on("oauth.deviceCode", (p) => {
      if (p.profileUuid !== req.profileUuid) {
        return;
      }
      prompt = p;
      phase = "waiting";
      startCountdown(p.expiresSec);
    });
    const offStatus = obs.on("oauth.status", () => void checkConnected());
    const offErr = obs.on("oauth.connectError", (p) => {
      if (p.profileUuid !== req.profileUuid) {
        return;
      }
      stopTimer();
      phase = "error";
      errorMsg = p.error || "Connection failed.";
    });
    return () => {
      offCode();
      offStatus();
      offErr();
      stopTimer();
    };
  });

  function onKeydown(e: KeyboardEvent) {
    if (e.key === "Escape") {
      onClose();
    }
  }
</script>

<svelte:window onkeydown={onKeydown} />

<div
  class="modal-backdrop"
  role="presentation"
  onclick={(e) => {
    if (e.target === e.currentTarget) onClose();
  }}
>
  <div class="modal" role="dialog" aria-modal="true" aria-label="Connect Account">
    <header class="modal-head">
      <h3>Connect {req.platformName}</h3>
      <button class="icon close" title="Close" onclick={onClose}>✕</button>
    </header>

    <div class="modal-body">
      {#if phase === "error"}
        <p class="error">{errorMsg}</p>
        <p class="dim">The connection could not be completed. Try again or close.</p>
      {:else if phase === "starting" || !prompt}
        <p class="dim">Starting connection…</p>
      {:else}
        <p class="dim">Enter this code in the page that just opened in your browser:</p>
        <div class="code">{prompt.userCode}</div>
        <p class="vu">
          <a href={prompt.verificationUri} target="_blank" rel="noreferrer noopener">{prompt.verificationUri}</a>
          <span class="muted"> (opened in your browser)</span>
        </p>
        <p class="waiting">
          Waiting for authorization…{#if remaining > 0}<span class="muted"> · code expires in {fmt(remaining)}</span>{/if}
        </p>
      {/if}
    </div>

    <footer class="modal-foot">
      {#if phase === "error"}
        <button class="btn" onclick={() => void begin()}>Retry</button>
      {/if}
      <button class="btn ghost" onclick={onClose}>Close</button>
    </footer>
  </div>
</div>

<style>
  .modal-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.55);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 100;
    padding: 24px;
  }
  .modal {
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    width: min(440px, 100%);
    max-height: 86vh;
    display: flex;
    flex-direction: column;
    box-shadow: 0 18px 48px rgba(0, 0, 0, 0.5);
    font-family: var(--font-ui);
  }
  .modal-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 8px 11px;
    background: var(--color-surface);
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .modal-head h3 {
    margin: 0;
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-text);
    font-weight: 600;
  }
  .modal-body {
    padding: 16px 14px;
    overflow: auto;
  }
  .modal-foot {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
    padding: 8px 11px;
    border-top: var(--border-weight) solid var(--color-border);
  }
  .code {
    font-family: var(--font-mono, monospace);
    font-size: 30px;
    letter-spacing: 0.18em;
    color: var(--color-accent);
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    text-align: center;
    padding: 14px 10px;
    margin: 10px 0;
    user-select: all;
  }
  .vu {
    margin: 0 0 12px;
    font-size: 12px;
    word-break: break-all;
  }
  .vu a {
    color: var(--color-accent);
    text-decoration: none;
  }
  .vu a:hover {
    text-decoration: underline;
  }
  .waiting {
    margin: 0;
    font-size: 12px;
    color: var(--color-text);
  }
  .muted {
    color: var(--color-muted);
  }
  .btn {
    height: auto;
    padding: 5px 10px;
    font-family: var(--font-ui);
    font-size: 11px;
    border: var(--border-weight) solid var(--color-border);
    background: transparent;
    color: var(--color-text);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    white-space: nowrap;
    cursor: pointer;
  }
  .btn:hover:not(:disabled) {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .btn.ghost {
    background: none;
  }
  .icon {
    background: none;
    border: none;
    color: var(--color-muted);
    cursor: pointer;
    padding: 2px 4px;
    font-size: 13px;
    line-height: 1;
    height: auto;
  }
  .icon:hover {
    color: var(--color-text);
  }
  .dim {
    color: var(--color-muted);
    margin: 0 0 4px;
    font-size: 12px;
  }
  .error {
    color: var(--color-live);
    margin: 0 0 8px;
    font-size: 12px;
  }
</style>
