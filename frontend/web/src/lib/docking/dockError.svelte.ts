// Per-dock error-reporting state. Each dock/composite that runs its own set of
// mutating obs.call(...) actions owns one instance (SourcesDock's failure and
// CanvasDock's failure are unrelated, so this is per-instance, not a shared
// singleton -- same reasoning as SourceSelection in sourceSelectionStore.svelte.ts).
// `report` is an arrow field (not a method) so it stays bound to its instance when
// passed directly as a `.catch(...)` callback rather than wrapped in a closure.
export class DockError {
  private _message = $state<string | null>(null);

  get message(): string | null {
    return this._message;
  }

  report = (e: unknown): void => {
    this._message = (e as Error).message;
  };

  clear(): void {
    this._message = null;
  }
}
