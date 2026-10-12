import { showToast, toast } from "$lib/stores/toastStore.svelte";

/** How long a failed dock action's toast stays up: a host error is a sentence to read, not
 *  the headline the toast's default interval is sized for. */
export const DOCK_ERROR_TOAST_MS = 6000;
/** The toast `kind` every dock action failure is shown under. */
export const DOCK_ERROR_TOAST_KIND = "dock-action-error";

// Per-dock error reporting, split by what the failure describes.
//
// `report` is for a user action that failed (rename, create, remove, reorder, paste,
// toggle, ...). That failure is an event, not a state of the dock: nothing on screen is
// wrong afterwards and nothing reliably comes along to retract it, so it is a toast. It
// is announced assertively because the user is waiting on the action they just took.
//
// `reportLoad` is for the dock's own content failing to load (its list read). That IS
// a state: the dock is now empty or stale, so it stays an in-dock banner (`message`)
// until the next successful load calls `clear`.
//
// Per-instance, not a shared singleton, because each dock's load state is its own --
// same reasoning as SourceSelection in sourceSelectionStore.svelte.ts. `report` and
// `reportLoad` are arrow fields (not methods) so they stay bound to their instance when
// passed directly as a `.catch(...)` callback rather than wrapped in a closure.
export class DockError {
  private _message = $state<string | null>(null);

  /** The load-failure banner text, or null while the dock's content loaded. */
  get message(): string | null {
    return this._message;
  }

  report = (e: unknown): void => {
    const message = (e as Error).message;
    // A failure repeated while it is still on screen (every pointer step of a failing
    // fader drag) would otherwise restart the toast's enter animation on each step.
    const shown = toast.current;
    if (shown?.kind === DOCK_ERROR_TOAST_KIND && shown.message === message) {
      return;
    }
    // The single-line toast truncates, so the hover title carries the full error.
    showToast(message, message, {
      assertive: true,
      dismissible: true,
      durationMs: DOCK_ERROR_TOAST_MS,
      kind: DOCK_ERROR_TOAST_KIND,
    });
  };

  reportLoad = (e: unknown): void => {
    this._message = (e as Error).message;
  };

  clear(): void {
    this._message = null;
  }
}
