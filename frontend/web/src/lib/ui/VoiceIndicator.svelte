<script lang="ts">
  import { obs } from "$lib/api/bridge";
  import { voiceStore } from "$lib/stores/voiceStore.svelte";
  import Button from "$lib/ui/Button.svelte";
  import { voiceIndicator } from "$lib/voice/voiceStatus";

  // Push-to-talk is invisible by nature: the user is looking at their game, not the app.
  // This cell is what tells them the key registered, and afterwards what was heard. It
  // renders nothing at all while voice is off. Wording and tone come from voiceIndicator.
  $effect(() => voiceStore.subscribe());

  const indicator = $derived(voiceIndicator(voiceStore.state));

  // A spoken yes is the point, but a hand already on the mouse should not have to speak.
  // Either call can lose a race with the pending window closing a moment earlier; the
  // host then refuses it and the next voice.state says what happened, so a refusal is
  // dropped rather than thrown into the console.
  function confirm(): void {
    void obs.call("voice.confirm").catch(() => {});
  }
  function cancel(): void {
    void obs.call("voice.cancel").catch(() => {});
  }
</script>

{#if indicator.visible}
  <!-- Status, not an alert: a live region so a screen reader hears the change, but polite,
       because it changes on every key press. -->
  <div class="voice" data-tone={indicator.tone}>
    <span
      class="state"
      role="status"
      aria-live="polite"
      title={indicator.detail ? `${indicator.label} — ${indicator.detail}` : indicator.label}
    >
      <span class="dot" aria-hidden="true"></span>
      <span class="label">{indicator.label}</span>
      {#if indicator.detail}
        <span class="detail">{indicator.detail}</span>
      {/if}
    </span>
    {#if indicator.confirmable}
      <Button size="xs" variant="filled" onclick={confirm}>Confirm</Button>
      <Button size="xs" onclick={cancel}>Cancel</Button>
    {/if}
  </div>
{/if}

<style>
  /* One full-height cell of the bottom bar's left cluster, divided like the viewer count
     beside it: same hairline, same mono face, same square dot as the focus dot. */
  .voice {
    display: flex;
    align-items: center;
    gap: 7px;
    min-width: 0;
    padding: 0 11px;
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-dim);
    white-space: nowrap;
    border-left: var(--border-weight) solid var(--color-border);
  }
  .state {
    display: flex;
    align-items: center;
    gap: 7px;
    min-width: 0;
  }
  .dot {
    width: 7px;
    height: 7px;
    flex: 0 0 auto;
    background: var(--color-muted);
  }
  .voice[data-tone="live"] .dot {
    background: var(--color-live);
  }
  .voice[data-tone="warn"] .dot {
    background: var(--color-warn);
  }
  .voice[data-tone="busy"] .dot {
    background: var(--color-accent);
    animation: pulse 1s ease-in-out infinite;
  }
  .label {
    color: var(--color-text);
  }
  .detail {
    min-width: 0;
    max-width: 28ch;
    overflow: hidden;
    text-overflow: ellipsis;
    color: var(--color-muted);
  }
  @keyframes pulse {
    50% {
      opacity: 0.4;
    }
  }
  /* The pulse is decorative; a user who asked for less motion gets a steady dot. */
  @media (prefers-reduced-motion: reduce) {
    .voice[data-tone="busy"] .dot {
      animation: none;
    }
  }
</style>
