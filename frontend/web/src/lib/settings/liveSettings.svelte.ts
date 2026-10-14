// A settings tab that applies each change as it is made (no Apply button): it reads the
// host's settings once, follows the host's push of them, and writes one change at a time,
// shown at once and rolled back if the host refuses it. The General and Voice tabs both
// work this way, and the skeleton was the same in both, down to the ordering rules below.
//
// The ordering rules: a reply is applied only if nothing newer has claimed the state since
// it was sent (RequestGuard), and the host's push outranks every reply still in flight. A
// refused change rolls back to what the host last CONFIRMED, not to what the control saw:
// with two changes in flight, the control's value already carries the first, which may be
// the one refused.
//
// Nothing is shown before the host has answered: there are no defaults here to render as
// if they were the user's settings. A read that fails says so (readError), and retry()
// asks again.
import { RequestGuard } from "$lib/utils/requestGuard";

export interface LiveSettingsSpec<S extends object, P> {
  read(): Promise<P>;
  write(patch: Partial<S>): Promise<P>;
  /** Follow the host's push of the full payload; returns the unsubscribe. */
  listen(onPush: (payload: P) => void): () => void;
  /** The settings in a payload (the payload itself when it is just the settings). */
  settingsOf(payload: P): S;
  /** Everything else a payload the host answered or pushed carries (for a tab to keep). */
  adopted?(payload: P): void;
  /** Words for a failure: the error's own, else the fallback. */
  errorText(error: unknown, fallback: string): string;
}

/** A change the host refused: which settings it was for, and why. */
export interface RefusedChange {
  keys: string[];
  message: string;
}

export class LiveSettings<S extends object, P = S> {
  #spec: LiveSettingsSpec<S, P>;
  #settings = $state<S | null>(null);
  #readError = $state<string | null>(null);
  #refused = $state<RefusedChange | null>(null);
  #confirmed: S | null = null;
  #guard = new RequestGuard();
  #active = false;

  constructor(spec: LiveSettingsSpec<S, P>) {
    this.#spec = spec;
  }

  /** What the controls show: the host's settings with any change in flight laid over.
   * null until the host has answered. */
  get settings(): S | null {
    return this.#settings;
  }

  /** Why the settings could not be read; null once they have been (or while asking). */
  get readError(): string | null {
    return this.#readError;
  }

  /** The last change the host refused, until the next change is made. */
  get refused(): RefusedChange | null {
    return this.#refused;
  }

  /** The reason the last refused change was refused, when it changed any of `keys`;
   * "" otherwise. For showing the reason beside the control it came from. */
  errorFor(...keys: (keyof S & string)[]): string {
    const refused = this.#refused;
    return refused && keys.some((k) => refused.keys.includes(k)) ? refused.message : "";
  }

  /** Read, and follow the host's push. Returns the stop, so a component can hand it to
   * an $effect. */
  start(): () => void {
    this.#active = true;
    this.#read();
    const off = this.#spec.listen((payload) => {
      // The host's own account of what it holds outranks every reply still in flight,
      // including a rollback about to restore an older value.
      this.#guard.supersede();
      this.#adopt(payload);
    });
    return () => {
      this.#active = false;
      off();
    };
  }

  /** Ask again after a failed read. */
  retry(): void {
    if (this.#active) {
      this.#read();
    }
  }

  /** Show a value without writing it (a slider being dragged); the change that follows,
   * or the host's next word, settles it. */
  show(patch: Partial<S>): void {
    if (this.#settings) {
      this.#settings = { ...this.#settings, ...patch };
    }
  }

  /** Apply one change: shown at once, written, and settled by the host's answer. A refusal
   * rolls back to what the host last confirmed and is kept in `refused`. Nothing happens
   * before the settings have been read: there is nothing yet to change. */
  async apply(patch: Partial<S>): Promise<void> {
    if (!this.#settings) {
      return;
    }
    this.#refused = null;
    const current = this.#guard.claim();
    this.#settings = { ...this.#settings, ...patch };
    try {
      const payload = await this.#spec.write(patch);
      if (current()) {
        this.#adopt(payload);
      }
    } catch (e) {
      this.#refused = { keys: Object.keys(patch), message: this.#spec.errorText(e, "The change was not applied.") };
      if (current() && this.#confirmed) {
        this.#settings = { ...this.#confirmed };
      }
    }
  }

  #read(): void {
    const current = this.#guard.claim();
    this.#readError = null;
    this.#spec.read().then(
      (payload) => {
        if (this.#active && current()) {
          this.#adopt(payload);
        }
      },
      (e) => {
        // A push that landed meanwhile already told us everything; only a read with
        // nothing to show for it is an error.
        if (this.#active && this.#settings === null) {
          this.#readError = this.#spec.errorText(e, "Could not read the settings.");
        }
      },
    );
  }

  #adopt(payload: P): void {
    const settings = this.#spec.settingsOf(payload);
    this.#settings = settings;
    this.#confirmed = { ...settings };
    this.#readError = null;
    this.#spec.adopted?.(payload);
  }
}
