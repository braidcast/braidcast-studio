<script lang="ts">
  import Icon from "$lib/ui/Icon.svelte";
  import { toast, hideToast } from "$lib/stores/toastStore.svelte";
  import { anyEscOwner, anyModalOpen, watchEscStack } from "$lib/utils/escStack";
  import { isEditable } from "$lib/utils/editableTarget";

  // Two live regions, both mounted for the app's lifetime with only their text toggled: a
  // region inserted together with its content is not reliably announced, and a role that flips
  // between status and alert under the reader is not reliably re-read either. A saved
  // screenshot must not interrupt speech the way a stream that refused to start should, so
  // each announcement goes to the region with the politeness it needs.
  let polite = $derived(toast.current && !toast.current.assertive ? toast.current.announce : "");
  let assertive = $derived(toast.current && toast.current.assertive ? toast.current.announce : "");

  // Clear of the GO-LIVE bar normally, but low when a modal is up: the bar is behind the
  // backdrop and unreachable then, while the modal's own footer -- carrying the button its
  // failure toasts are telling the streamer to press -- sits exactly where the raised toast
  // would land. Tracked rather than read once, so a modal closing under a live toast moves it
  // back off the bar.
  let modalOpen = $state(anyModalOpen());
  $effect(() => watchEscStack(() => (modalOpen = anyModalOpen())));

  let toastEl = $state<HTMLDivElement | null>(null);
  // The toast currently on screen, held separately so its focus hook is still reachable after
  // `toast.current` has moved on to the next one (or to nothing).
  let shownSeq = -1;
  let shownFocusReturn: (() => void) | undefined;
  let refocusToast = false;
  let refocusFallback: (() => void) | undefined;

  // Every way a toast goes away -- close button, Escape, the auto-dismiss timer, a retraction,
  // or a newer toast replacing it -- comes through here, and focus moves only if the toast is
  // holding it at that moment.
  //
  // .pre, because this is only answerable before the DOM update: once the outgoing node is
  // detached, `contains(document.activeElement)` is false in exactly the case it must be true.
  //
  // hasFocus() gates it because Chromium keeps activeElement across a window blur, so an
  // alt-tabbed streamer still looks like they are holding the close button. Restoring for
  // someone who is not there buys them nothing and leaves GO LIVE primed for whatever they
  // press on return; dropping to <body> costs them only a re-orient they were making anyway.
  $effect.pre(() => {
    const cur = toast.current;
    const seq = cur?.seq ?? -1;
    if (seq === shownSeq) {
      return;
    }
    const held = (toastEl?.contains(document.activeElement) ?? false) && document.hasFocus();
    const back = shownFocusReturn;
    const replaced = cur !== null;
    shownSeq = seq;
    shownFocusReturn = cur?.onFocusReturn;
    if (!held) {
      return;
    }
    // A replacement is not the toast going away -- one is still on screen, and {#key} merely
    // rebuilt the node under the user. Keep them on it rather than throwing them back into the
    // studio; only a teardown to nothing hands focus straight to the pusher's choice.
    if (replaced) {
      refocusToast = true;
      refocusFallback = back;
    } else {
      back?.();
    }
  });

  // The other half of that branch, after the DOM update has built the replacement node. Reading
  // `toastEl` is what subscribes this to the rebuild.
  $effect(() => {
    const el = toastEl;
    if (!refocusToast) {
      return;
    }
    refocusToast = false;
    const fallback = refocusFallback;
    refocusFallback = undefined;
    const next = el?.querySelector<HTMLButtonElement>("button");
    if (next) {
      next.focus();
    } else {
      // The replacement carries nothing focusable -- a plain refusal toast landing on top of a
      // dismissible one. Hand focus to the outgoing toast's choice rather than losing it: for a
      // start failure that is GO LIVE, which is where the streamer is already waiting.
      fallback?.();
    }
  });

  // The mount point sits after every page and dialog, which makes the close button the last
  // focusable element in the document -- reachable in principle, not in the seconds a toast
  // lives. Escape is the affordance that actually works. Deliberately the lowest-priority
  // owner: a dialog, modal or menu the user opened consumes the key first (escStack), a text
  // field keeps its own cancel, and the key is not swallowed either way.
  $effect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== "Escape" || toast.current === null || anyEscOwner() || isEditable(e.target)) {
        return;
      }
      userDismiss();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  });

  // The only two ways the user says "I am done with this", and so the only two that may let the
  // pusher discard what the toast was describing. Expiry, retraction and replacement must not.
  function userDismiss(): void {
    const after = toast.current?.onUserDismiss;
    hideToast();
    after?.();
  }

  // Taking the action ends the toast: it reported something, the user answered it, and a
  // notice still offering an undo that has already been taken would invite a second press.
  // Not onUserDismiss -- that one means "I am done with this", which is the opposite of
  // having acted on it, and the pusher uses it to discard exactly the state an undo needs.
  function takeAction(): void {
    const act = toast.current?.action?.onAction;
    hideToast();
    act?.();
  }
</script>

<span class="sr-only" role="status">{polite}</span>
<span class="sr-only" role="alert">{assertive}</span>

{#if toast.current}
  {#key toast.current.seq}
    <div
      bind:this={toastEl}
      class="toast"
      class:rich={toast.current.lines.length > 0}
      style:--toast-bottom={modalOpen ? "24px" : "68px"}
      title={toast.current.lines.length > 0 ? undefined : toast.current.title}
    >
      <div class="body">
        <span class="msg">{toast.current.message}</span>
        {#each toast.current.lines as line, i (i)}
          <span class="line">{line}</span>
        {/each}
      </div>
      {#if toast.current.action}
        <button class="action" type="button" onclick={takeAction}>{toast.current.action.label}</button>
      {/if}
      {#if toast.current.dismissible}
        <button class="close" type="button" aria-label="Dismiss notification" onclick={userDismiss}>
          <Icon name="x" size={13} />
        </button>
      {/if}
    </div>
  {/key}
{/if}

<style>
  /* Fixed, so nothing here participates in page layout: the studio behind it neither reflows
     nor resizes when a toast appears. Raised clear of the 52px GO-LIVE bar, which carries the
     live badge, the stats strip and the button that ends the stream -- except behind a modal,
     where the script drops it back down to clear the dialog's action bar instead. */
  .toast {
    position: fixed;
    left: 50%;
    bottom: var(--toast-bottom, 68px);
    transform: translateX(-50%);
    z-index: 300;
    display: flex;
    align-items: flex-start;
    gap: 9px;
    max-width: min(560px, calc(100vw - 48px));
    padding: 9px 14px;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-accent);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    box-shadow: 0 4px 18px rgba(0, 0, 0, 0.4);
    animation: toast-in 140ms ease-out;
  }

  .toast.rich {
    max-width: min(640px, calc(100vw - 48px));
    line-height: 1.45;
  }

  .body {
    display: flex;
    flex-direction: column;
    gap: 2px;
    min-width: 0;
  }

  /* A single-line toast keeps its old one-line shape; extra lines mean the text is worth
     reading in full, so that variant wraps instead of clipping. */
  .msg {
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
  }

  .rich .msg {
    white-space: normal;
  }

  .line {
    color: var(--color-dim);
  }

  /* Reads as the one thing to press here: accent text against the toast's own muted detail,
     floored at WCAG 2.5.8's 24px by padding rather than by type size, and never wrapped --
     a two-line "Undo" beside a message that is already ellipsing is unreadable. Aligned to
     the first line, since the body beside it can be several. */
  .action {
    flex: 0 0 auto;
    display: inline-flex;
    align-items: center;
    min-height: 24px;
    margin: -3px 0;
    padding: 0 6px;
    background: none;
    border: 0;
    color: var(--color-accent);
    font: inherit;
    letter-spacing: inherit;
    white-space: nowrap;
    cursor: pointer;
  }

  .action:hover {
    background: color-mix(in srgb, var(--color-accent) 14%, transparent);
  }

  .action:focus-visible {
    outline: var(--border-weight) solid var(--color-accent);
    outline-offset: 1px;
  }

  .close {
    flex: 0 0 auto;
    display: grid;
    place-items: center;
    width: 24px;
    height: 24px;
    margin: -3px -6px -3px 0;
    padding: 0;
    color: var(--color-dim);
    background: none;
    border: 0;
    cursor: pointer;
  }

  .close:hover {
    color: var(--color-text);
    background: color-mix(in srgb, var(--color-text) 12%, transparent);
  }

  .close:focus-visible {
    outline: var(--border-weight) solid var(--color-accent);
    outline-offset: 1px;
  }

  @keyframes toast-in {
    from {
      opacity: 0;
      transform: translate(-50%, 8px);
    }
    to {
      opacity: 1;
      transform: translate(-50%, 0);
    }
  }

  @media (prefers-reduced-motion: reduce) {
    .toast {
      animation: none;
    }
  }
</style>
