<script lang="ts">
  import Modal from "$lib/ui/Modal.svelte";
  import { obs, type DeviceCodeProgress } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { streamProfileStore } from "$lib/stores/streamProfileStore.svelte";
  import { oauthStore } from "$lib/stores/oauthStore.svelte";
  import { markOAuthConnected, type OAuthConnectRequest } from "$lib/dialogs/oauthConnectOpener.svelte";

  interface Props {
    req: OAuthConnectRequest;
    onClose: () => void;
  }
  let { req, onClose }: Props = $props();

  // The native preview overlay is suspended by the opener for this dialog's whole
  // lifetime, so it never occludes the modal.

  type Phase = "starting" | "waiting" | "error";
  // Which auth strategy the in-flight connect resolved to, set from the progress
  // event's `phase`. null until the first progress event arrives.
  type ConnMode = "deviceCode" | "browser";
  let phase = $state<Phase>("starting");
  let connMode = $state<ConnMode | null>(null);
  let prompt = $state<DeviceCodeProgress | null>(null);
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

  // Kick off (or retry) the connect flow. The call returns immediately
  // ({pending:true}); the strategy + its details arrive via oauth.connectProgress.
  async function begin() {
    phase = "starting";
    connMode = null;
    errorMsg = null;
    prompt = null;
    stopTimer();
    // Cancel any prior in-flight attempt first so a second worker can't stack on
    // the first (a fresh connect supersedes it, but the stale worker lingers).
    try {
      await obs.call("oauth.cancelConnect");
    } catch {
      // Older host without cancelConnect — ignore and proceed.
    }
    try {
      await obs.call("oauth.connect", { providerId: req.providerId, profileUuid: req.profileUuid });
    } catch (e) {
      phase = "error";
      errorMsg = (e as Error).message;
    }
  }

  // Detect this profile's account going connected off the shared stores. Status rows
  // are keyed by accountId (set by the backend on a successful grant), so resolve this
  // profile's linked accountId and match on it. Reacting to the stores (rather than
  // re-fetching on each event) closes the link/status emit race the old code noted:
  // the effect re-runs when EITHER the profile's accountId or the status arrives.
  $effect(() => {
    const accountId = streamProfileStore.byUuid(req.profileUuid)?.accountId;
    if (accountId && oauthStore.statuses.some((s) => s.accountId === accountId && s.connected)) {
      markOAuthConnected();
      onClose();
    }
  });

  $effect(() => {
    void begin();
    streamProfileStore.start();
    const offOauth = oauthStore.subscribe();
    const offProgress = obs.on(EV.oauthConnectProgress, (p) => {
      if (p.profileUuid !== req.profileUuid) {
        return;
      }
      phase = "waiting";
      if (p.phase === "deviceCode") {
        connMode = "deviceCode";
        prompt = p;
        startCountdown(p.expiresSec);
      } else {
        // PKCE loopback: no code, but the loopback self-cancels at `timeoutSec`, so
        // drive the same countdown the device-code phase uses (older hosts omit it →
        // no deadline, plain wait).
        connMode = "browser";
        prompt = null;
        const timeoutSec = p.timeoutSec ?? 0;
        if (timeoutSec > 0) {
          startCountdown(timeoutSec);
        } else {
          stopTimer();
        }
      }
    });
    const offErr = obs.on(EV.oauthConnectError, (p) => {
      if (p.profileUuid !== req.profileUuid) {
        return;
      }
      stopTimer();
      phase = "error";
      errorMsg = p.error || "Connection failed.";
    });
    return () => {
      offProgress();
      offOauth();
      offErr();
      stopTimer();
    };
  });
</script>

<Modal title="Connect {req.platformName}" {onClose} width={440}>
  {#if phase === "error"}
    <p class="error">{errorMsg}</p>
    <p class="dim">The connection could not be completed. Try again or close.</p>
  {:else if phase === "waiting" && connMode === "browser"}
    <div class="browser-wait">
      <div class="spinner" role="status" aria-label="Waiting for authorization"></div>
      <p class="waiting">
        Waiting for browser sign-in{#if remaining > 0}<span class="muted"> — times out in {fmt(remaining)}</span
          >{:else}…{/if}
      </p>
    </div>
    <p class="dim">The browser was opened automatically — complete authorization there.</p>
  {:else if phase === "waiting" && connMode === "deviceCode" && prompt}
    <p class="dim">Enter this code in the page that just opened in your browser:</p>
    <div class="code">{prompt.userCode}</div>
    <p class="vu">
      <a href={prompt.verificationUri} target="_blank" rel="noreferrer noopener">{prompt.verificationUri}</a>
      <span class="muted"> (opened in your browser)</span>
    </p>
    <p class="waiting">
      Waiting for authorization…{#if remaining > 0}<span class="muted"> · code expires in {fmt(remaining)}</span>{/if}
    </p>
  {:else}
    <p class="dim">Starting connection…</p>
  {/if}

  {#snippet footer()}
    {#if phase === "error"}
      <button class="ghost" onclick={onClose}>Close</button>
      <button class="btn" onclick={() => void begin()}>Retry</button>
    {:else}
      <button class="btn" onclick={onClose}>
        {phase === "waiting" || phase === "starting" ? "Cancel" : "Close"}
      </button>
    {/if}
  {/snippet}
</Modal>

<style>
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
  .browser-wait {
    display: flex;
    align-items: center;
    gap: 12px;
    margin: 6px 0 10px;
  }
  .spinner {
    flex: none;
    width: 18px;
    height: 18px;
    border: 2px solid var(--color-border);
    border-top-color: var(--color-accent);
    animation: oauth-spin 0.8s linear infinite;
  }
  @keyframes oauth-spin {
    to {
      transform: rotate(360deg);
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .spinner {
      animation-duration: 2.4s;
    }
  }
  .muted {
    color: var(--color-muted);
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
