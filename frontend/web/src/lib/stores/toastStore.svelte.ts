// Minimal transient-toast queue. A single notification is surfaced at the bottom
// of the app and auto-dismisses after DISMISS_MS; showToast replaces whatever is
// shown (no stacking). `seq` lets the view re-key so a repeated message restarts
// its enter animation. App owns the single Toast mount; any module calls showToast.

export interface ToastState {
  /** The line shown in the toast (kept short; the full text lives in `title`). */
  message: string;
  /** Hover text — the untruncated detail (e.g. the full screenshot path). */
  title: string;
  seq: number;
}

const DISMISS_MS = 4000;
let timer: ReturnType<typeof setTimeout> | undefined;

export const toast = $state<{ current: ToastState | null }>({ current: null });

/** Show a transient toast, replacing any current one and resetting the timer. */
export function showToast(message: string, title: string): void {
  toast.current = { message, title, seq: (toast.current?.seq ?? 0) + 1 };
  clearTimeout(timer);
  timer = setTimeout(() => (toast.current = null), DISMISS_MS);
}

export function dismissToast(): void {
  clearTimeout(timer);
  toast.current = null;
}
