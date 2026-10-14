<script lang="ts">
  import { voiceStore } from "$lib/stores/voiceStore.svelte";
  import Button from "$lib/ui/Button.svelte";
  import { cancelVoice, confirmVoice } from "$lib/voice/voiceActions";
  import { voiceIndicator } from "$lib/voice/voiceStatus";

  // Push-to-talk is invisible by nature: the user is looking at their game, not the app.
  // This cell is what tells them the key registered, and afterwards what was heard. It
  // renders nothing at all while voice is off. Wording and tone come from voiceIndicator.
  $effect(() => voiceStore.subscribe());

  const indicator = $derived(voiceIndicator(voiceStore.state));

  // A spoken yes is the point, but a hand already on the mouse should not have to speak.
  // Each click names the action it was made for, read when it is made: one that lands
  // after that action is gone is refused and acts on nothing, and the next voice.state
  // says what happened.
  function confirm(): void {
    void confirmVoice(indicator.pendingId);
  }
  function cancel(): void {
    void cancelVoice(indicator.pendingId);
  }

  // Escape on the cell's buttons cancels, as Cancel does: the voice cancel key does not act
  // while the app is in front, so in the app Escape is the UI's to handle, and only where
  // it can mean nothing else. It goes no further.
  let cell = $state<HTMLElement | null>(null);
  $effect(() => {
    const el = cell;
    if (!el) {
      return;
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== "Escape" || e.defaultPrevented || !indicator.cancellable) {
        return;
      }
      e.preventDefault();
      e.stopPropagation();
      cancel();
    };
    el.addEventListener("keydown", onKey);
    return () => el.removeEventListener("keydown", onKey);
  });
</script>

{#if indicator.visible}
  <!-- Status, not an alert: a live region so a screen reader hears the change, but polite,
       because it changes on every key press. -->
  <div class="voice" data-tone={indicator.tone} bind:this={cell}>
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
    {#if indicator.cancellable}
      <span class="acts">
        {#if indicator.confirmable}
          <Button size="xs" variant="filled" onclick={confirm}>Confirm</Button>
        {/if}
        <Button size="xs" onclick={cancel}>Cancel</Button>
      </span>
    {/if}
  </div>
{/if}

<style>
  /* One full-height cell of the bottom bar's left cluster, divided like the viewer count
     beside it: same hairline, same mono face, same square dot as the focus dot. */
  /* It is the cell that gives way when the bar runs short: a dictated draft's summary can
     be a whole message long. The text shrinks, down to nothing if it must, and the buttons
     never do, so Confirm and Cancel stay on screen (the title keeps the full text). */
  .voice {
    display: flex;
    flex: 0 1 auto;
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
    flex: 0 1 auto;
    align-items: center;
    gap: 7px;
    min-width: 0;
    overflow: hidden;
  }
  .acts {
    display: flex;
    flex: 0 0 auto;
    gap: 7px;
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
    flex: 0 1 auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    color: var(--color-text);
  }
  .detail {
    flex: 0 2 auto;
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
