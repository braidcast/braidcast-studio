import { obs, type BrowserDock } from "./bridge";

// Reactive store for the user-defined browser docks (Task 12). The full list is
// the single source of truth: add/update/remove mutate it locally then persist
// the whole list via browserDocks.set (which echoes the saved list back). The
// StudioPage reconciler reacts to `docks` changing to open/close iframe panels.
//
// Open/closed model: every stored browser dock is shown as a panel — add = open +
// persist, remove = close + delete. There is no separate open/closed state.

// Monotonic counter fallback for id generation when crypto.randomUUID is absent
// (some CEF/embedded contexts gate it); combined with the title for readability.
let idCounter = 0;
function genId(title: string): string {
  const uuid = globalThis.crypto?.randomUUID?.();
  if (uuid) {
    return uuid;
  }
  idCounter += 1;
  return title.replace(/\s+/g, "-").toLowerCase() + "-" + idCounter;
}

class BrowserDockStore {
  docks = $state<BrowserDock[]>([]);
  // Reactive so consumers can gate a reconcile on the list having actually loaded.
  // Flips true only AFTER the fetch settles (not when it starts), so `docks` is
  // populated by the time gated readers run.
  loaded = $state(false);
  #loading = false;

  // Load once on first use; idempotent. Callers can await or fire-and-forget.
  async load(): Promise<void> {
    if (this.loaded || this.#loading) {
      return;
    }
    this.#loading = true;
    try {
      this.docks = await obs.call("browserDocks.list");
    } catch {
      this.docks = [];
    } finally {
      this.#loading = false;
      this.loaded = true;
    }
  }

  private async persist(): Promise<void> {
    try {
      this.docks = await obs.call("browserDocks.set", { docks: this.docks });
    } catch {
      // Keep the optimistic local list; the next mutation retries the write.
    }
  }

  async add(dock: { title: string; url: string }): Promise<void> {
    this.docks = [...this.docks, { id: genId(dock.title), title: dock.title, url: dock.url }];
    await this.persist();
  }

  async update(id: string, patch: Partial<Omit<BrowserDock, "id">>): Promise<void> {
    this.docks = this.docks.map((d) => (d.id === id ? { ...d, ...patch } : d));
    await this.persist();
  }

  async remove(id: string): Promise<void> {
    this.docks = this.docks.filter((d) => d.id !== id);
    await this.persist();
  }

  byId(id: string): BrowserDock | undefined {
    return this.docks.find((d) => d.id === id);
  }
}

export const browserDockStore = new BrowserDockStore();
