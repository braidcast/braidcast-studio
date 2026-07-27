import { obs, type BrowserDock } from "$lib/api/bridge";
import { showToast } from "$lib/stores/toastStore.svelte";
import { log } from "$lib/utils/log";
import { Cat } from "$lib/utils/logCategories";

// Reactive store for the user-defined browser docks (Task 12). The full list is
// the single source of truth: add/update/remove route through one write path that
// persists the whole list via browserDocks.set (which echoes the saved list back).
// The StudioPage reconciler reacts to `docks` changing to open/close iframe panels.
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
  #loadFailed = $state(false);

  /** True while the saved list has never been read successfully, which suspends every
   * write. Consumers surface this and offer `retryLoad`. */
  readonly loadFailed = $derived(this.#loadFailed);

  // Load once on first use; idempotent. Callers can await or fire-and-forget.
  async load(): Promise<void> {
    if (this.loaded || this.#loading) {
      return;
    }
    await this.#fetch();
  }

  /** Re-attempt a load that failed. Resolves true once the list is readable again,
   * which re-enables persistence. */
  async retryLoad(): Promise<boolean> {
    if (!this.#loading) {
      await this.#fetch();
    }
    return !this.#loadFailed;
  }

  async #fetch(): Promise<void> {
    this.#loading = true;
    try {
      this.docks = await obs.call("browserDocks.list");
      this.#loadFailed = false;
    } catch (e) {
      this.docks = [];
      this.#loadFailed = true;
      log.warn(Cat.bridge, "browserDocks.list failed:", (e as Error).message);
    } finally {
      this.#loading = false;
      this.loaded = true;
    }
  }

  // The single write path. browserDocks.set rebuilds browser_docks.json wholesale
  // from this payload, so writing a list the app never read would erase every dock
  // this session never saw; the mutation is dropped whole rather than half-applied.
  async #commit(next: BrowserDock[]): Promise<void> {
    if (this.#loadFailed) {
      showToast(
        "Browser docks are read-only until the saved list loads",
        "The saved browser docks could not be read, so changes are not being written — " +
          "saving now would erase them. Retry from Settings → Browser Docks.",
      );
      return;
    }
    this.docks = next;
    try {
      this.docks = await obs.call("browserDocks.set", { docks: next });
    } catch (e) {
      // Keep the optimistic local list so the panel the user just opened does not
      // vanish under them; the next mutation retries the write. Nothing else would
      // reveal that the change is only in memory until it is missing on restart.
      const msg = (e as Error).message;
      log.warn(Cat.bridge, "browserDocks.set failed:", msg);
      showToast("Browser dock change not saved", msg);
    }
  }

  async add(dock: { title: string; url: string }): Promise<void> {
    await this.#commit([...this.docks, { id: genId(dock.title), title: dock.title, url: dock.url }]);
  }

  async update(id: string, patch: Partial<Omit<BrowserDock, "id">>): Promise<void> {
    await this.#commit(this.docks.map((d) => (d.id === id ? { ...d, ...patch } : d)));
  }

  async remove(id: string): Promise<void> {
    await this.#commit(this.docks.filter((d) => d.id !== id));
  }

  byId(id: string): BrowserDock | undefined {
    return this.docks.find((d) => d.id === id);
  }
}

export const browserDockStore = new BrowserDockStore();
