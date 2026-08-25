import { obs } from "$lib/api/bridge";
import { log } from "$lib/utils/log";
import { Cat } from "$lib/utils/logCategories";

// The installed font families, read once per session and shared by every font field.
//
// One store rather than a fetch per control: the properties view and the overlay fields
// panel both render font inputs repeatedly, and the host answers each call by handing
// back the same cached enumeration -- so N controls asking separately is N round trips
// for one identical array.
//
// The in-flight promise is retained rather than an "already loading" boolean because it
// is the only shape a second caller can AWAIT. A boolean guard collapses the calls just
// as well (browserDockStore does exactly that), but its late callers return immediately
// while the first fetch is still open, so `await load()` there does not mean the list has
// arrived. Nothing needs that yet -- the datalist just re-renders when `families` lands --
// but a store whose load() lies about being finished is a trap to leave lying around.
class FontStore {
  families = $state<string[]>([]);

  // Retained once it FULFILLS, so `load()` is a no-op for the rest of the session. A
  // rejection clears it instead and the next font control to mount tries again -- which
  // is the whole retry policy: no timer, no backoff, nothing for a caller to drive.
  //
  // Clearing matters because neither reject path is permanent. A host that could not read
  // the font collection does not reject at all: it answers `{families: []}` and the field
  // simply offers nothing. Rejection means the bridge was unreachable -- no
  // window.cefQuery yet, or a query cut off by teardown -- and both of those pass, so
  // caching one would leave every font field in the session suggestionless over a
  // condition that had already resolved, with a single log line as the only trace.
  #inFlight: Promise<void> | null = null;

  /** Idempotent; callers can await or fire-and-forget. */
  load(): Promise<void> {
    this.#inFlight ??= this.#fetch();
    return this.#inFlight;
  }

  async #fetch(): Promise<void> {
    try {
      const { families } = await obs.call("fonts.list");
      this.families = families;
    } catch (e) {
      // Only reachable after the await, so `load()` has already stored the promise this
      // clears -- there is no window in which the assignment lands after the reset.
      // Swallowed rather than rethrown: callers fire-and-forget, and a rejection nobody
      // awaits is an unhandled one. `families` keeps its last good value, which before
      // any success is the empty array it starts as.
      this.#inFlight = null;
      log.warn(Cat.bridge, "fonts.list failed:", (e as Error).message);
    }
  }
}

export const fontStore = new FontStore();
