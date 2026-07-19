// One-shot bridge for the app-level F2 shortcut. `onKeydown` lives in App.svelte but the
// inline-rename editors live inside the docks, so F2 pokes this signal with a target and the
// owning dock's $effect calls its EXISTING beginRename for the matching row. No rename UI or
// bridge call lives here -- only the request hand-off.
export type RenameTarget = { kind: "source"; id: number } | { kind: "scene"; name: string };

class RenameSignal {
  // Latest request; seq bumps every poke so a repeated F2 on the same target still re-fires
  // the dock effect (fresh object == new reference) and each dock acts at most once per seq.
  pending = $state<{ target: RenameTarget; seq: number } | null>(null);
  private seq = 0;

  request(target: RenameTarget): void {
    this.pending = { target, seq: ++this.seq };
  }
}

export const renameSignal = new RenameSignal();
