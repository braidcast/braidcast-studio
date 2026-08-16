// Minimal transient-toast queue. A single notification is surfaced above the studio's
// bottom bar and auto-dismisses after its interval; showToast replaces whatever is
// shown (no stacking). `seq` lets the view re-key so a repeated message restarts its
// enter animation. App owns the single Toast mount; any module calls showToast.

export interface ToastOptions {
  /** Extra lines under the headline. Their presence switches the toast from one
   *  truncating line to a wrapping block. */
  lines?: string[];
  /** What a screen reader hears. Defaults to `message`; pass a shorter summary when the
   *  visible text carries more detail than anyone wants read out at the moment it lands. */
  announce?: string;
  /** Route the announcement through the assertive live region instead of the polite one:
   *  a failure the user is waiting on interrupts, a confirmation waits its turn. Carries no
   *  visual weight -- every toast looks the same. */
  assertive?: boolean;
  /** Render a dismiss button. */
  dismissible?: boolean;
  durationMs?: number;
  /** Identifies the pusher so it can withdraw its own toast without swallowing a newer
   *  one someone else put up in the meantime. */
  kind?: string;
  /** Where focus goes when the toast is torn down while holding it, in a focused window. Fires
   *  on every teardown that leaves NO toast on screen: close button, Escape, expiry, retraction.
   *  A replacement instead moves focus to the incoming toast's dismiss button, and falls back to
   *  this only when that toast has none. Move focus here and nothing else: most of these
   *  teardowns are not the user finishing with the toast. */
  onFocusReturn?: () => void;
  /** The user deliberately got rid of it: the close button or Escape, nothing else. Never
   *  fires on expiry, retraction, or replacement, which makes it the only safe place to
   *  discard the state the toast was describing. */
  onUserDismiss?: () => void;
}

export interface ToastState {
  /** The headline shown in the toast. */
  message: string;
  /** Hover text for the single-line variant, whose message truncates — the untruncated detail
   *  (e.g. the full screenshot path). A toast with `lines` wraps and shows everything, so the
   *  view drops the tooltip there rather than repeating visible text back at the pointer. */
  title: string;
  seq: number;
  lines: string[];
  announce: string;
  assertive: boolean;
  dismissible: boolean;
  kind: string;
  onFocusReturn?: () => void;
  onUserDismiss?: () => void;
}

const DEFAULT_MS = 4000;
let timer: ReturnType<typeof setTimeout> | undefined;
// Monotonic rather than derived from the current toast: clearing to null would otherwise
// restart the count and hand a later toast a number the view has already keyed on.
let seq = 0;

export const toast = $state<{ current: ToastState | null }>({ current: null });

/** Show a transient toast, replacing any current one and resetting the timer. */
export function showToast(message: string, title: string, opts: ToastOptions = {}): void {
  toast.current = {
    message,
    title,
    seq: ++seq,
    lines: opts.lines ?? [],
    announce: opts.announce ?? message,
    assertive: opts.assertive ?? false,
    dismissible: opts.dismissible ?? false,
    kind: opts.kind ?? "",
    onFocusReturn: opts.onFocusReturn,
    onUserDismiss: opts.onUserDismiss,
  };
  clearTimeout(timer);
  timer = setTimeout(() => {
    timer = undefined;
    toast.current = null;
  }, opts.durationMs ?? DEFAULT_MS);
}

/** Withdraw a toast before its timer runs out. With `kind`, only when that pusher's toast
 *  is the one showing — retracting a notice that no longer describes anything must not
 *  take a newer message down with it. */
export function hideToast(kind?: string): void {
  if (kind !== undefined && toast.current?.kind !== kind) {
    return;
  }
  clearTimeout(timer);
  timer = undefined;
  toast.current = null;
}
