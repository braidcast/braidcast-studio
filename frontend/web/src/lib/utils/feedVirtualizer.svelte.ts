// Shared virtualized, paged feed engine for the Multichat and Events docks. Both render a
// bottom-sticky, absolutely-positioned virtual list measured row by row over a window of
// the host's history: the dock opens on the newest few screens, older pages load on their
// own as the reader scrolls up (no "show earlier" control), and the window is trimmed back
// while the reader sits at the bottom. Scrolled up, it is capped to hardMax rows: older
// pages still load, and the newest end is let go instead, like a messenger unloading the
// far end of a long conversation, never a row the reader can see. The machinery --
// rAF-batched appends, the paged window, per-row measurement, visible-range windowing,
// auto-stick-to-bottom and scroll anchoring -- lives here once.
//
// The docks diverge only in configuration: row-height estimate, order (config.compare,
// which must equal the host's page order), item identity (config.key), window sizes, and
// the fetch, which closes over the dock's current destination filter -- filtering happens
// on the host, so every row held matches the filter and a filter switch is a load().
//
// A consumer constructs one instance in its <script> (so the internal effects bind to the
// component), renders `visible` over a `layout.total`-high sizer, and wires the two
// actions (`scroll` on the scroll container, `measureRow` per row). `display` leads with
// a `top` row of fixed height, which reads "loading", nothing, or "start of history", and
// interleaves a fixed-height divider where each broadcast began (config.boundaries).
//
// Scroll anchoring: rows sit at `style:top` over a sizer, which suppresses CSS scroll
// anchoring, so this does it by hand. While the reader is scrolled up, every layout
// change (a page prepended, a row measured, a row inserted above, a window replaced) puts
// the row under the reader's eye back at the same viewport offset.

import { untrack } from "svelte";
import type { FeedPage } from "$lib/api/bridge";
import { RequestGuard } from "$lib/utils/requestGuard";

// A failed load or older page is asked again after this long, doubling per failure up to
// the cap.
const RETRY_MS = 1000;
const RETRY_MAX_MS = 30_000;

// How long the feed remembers a patch for items that join the window after it (see
// patch). Only an item the host held before the patch but that arrives after it needs
// one. A page read before it is covered for as long as that page is in flight, however
// long; a live frame the host posted late is covered by time. The latest trails its patch
// by at most the overlay's 3 s send timeout per stalled reader (chat_hub.cpp holds the
// frame in BroadcastChat), so 10 s leaves room for a few.
const PATCH_RETAIN_MS = 10_000;
// And the most it remembers at once: 10 s of a mass-ban raid at about 400 bans a second.
// Past it the oldest goes regardless, and an item it would have caught keeps its old form
// until the next load, which reads the host's patched copy.
const PATCH_MEMORY_MAX = 4096;

/** What the top row says: a page is loading, older rows are there to load, the window
 * reaches the oldest row the host holds, or the newest page failed and is being asked
 * for again. */
export type TopState = "loading" | "idle" | "start" | "retrying";

/** The top row's key. Item keys count up from 1 and divider keys down from -2, so
 * neither can take it. */
export const TOP_KEY = -1;

/** A moment the feed marks with a divider: a broadcast start, by session id. */
export interface Boundary {
  id: string;
  at: number;
  /** The broadcast is still running, so its divider shows even before anything follows it. */
  live: boolean;
}

export type DisplayRow<T> =
  | { kind: "item"; clientKey: number; item: T }
  | { kind: "divider"; clientKey: number; at: number; label: string }
  | { kind: "top"; clientKey: -1; state: TopState };

type ItemRow<T> = Extract<DisplayRow<T>, { kind: "item" }>;

/** The rows that are not items: the top row and the dividers. */
export type MarkerRow = Exclude<DisplayRow<never>, { kind: "item" }>;

export interface FeedConfig<T> {
  /** Px height estimate for an unmeasured item row. */
  estimate: number;
  /** Rows rendered beyond the viewport on each side (default 6). */
  overscan?: number;
  /** Within this many px of the bottom counts as "stuck to latest" (default 24). */
  stickPx?: number;
  /** The top row's fixed CSS height. */
  topHeight: number;
  /** Stable identity; a row keeps its clientKey (its measured height and its hold on the
   * reader's scroll position) for as long as an item with its key is in the window. */
  key(item: T): string;
  /** The host's page order. */
  compare(a: T, b: T): number;
  /** When the item happened, on the clock `boundaries` are measured on. */
  timeOf(item: T): number;
  /** One page, oldest-first: the newest without `before`, else the rows just older. */
  fetch(q: { before?: T; limit: number }): Promise<FeedPage<T>>;
  /** Rows a load asks for and a trim keeps (raised to three screens when that is more). */
  base: number;
  /** Rows an older page asks for. */
  page: number;
  /** Rows past which a window stuck to the bottom is trimmed back to base. */
  highWater: number;
  /** The rows a window the reader has scrolled up in is capped to. Past it rows are let go
   * from the newest end first, and the window detaches: a live row newer than its last is
   * only counted as unseen, and reaching its bottom again reloads the newest page. What that
   * end cannot cover goes from the oldest end. A row the viewport shows is never let go, so
   * a window whose visible rows leave too few outside them stays over the cap. */
  hardMax: number;
  /** An older page loads once the viewport top is within this many screens of the window's
   * top (default 1.5). */
  prefetchScreens?: number;
  /** Where to draw dividers, in any order. Read inside a derivation, so a reactive source
   * redraws them. */
  boundaries?: () => readonly Boundary[];
  dividerLabel?: (b: Boundary) => string;
  /** A divider's fixed CSS height (default topHeight). */
  dividerHeight?: number;
}

interface Layout {
  tops: number[];
  total: number;
}

// Where the reader is, in terms that survive a layout change: the rows the viewport
// showed, each with the offset of the viewport top from that row's top. `at` is the
// scrollTop this was taken at, and `rows`/`layout` the display it was taken against,
// so a user scroll that lands before its scroll event can be re-read against the
// layout the user actually saw.
interface Anchor<T> {
  at: number;
  rows: DisplayRow<T>[];
  layout: Layout;
  marks: { key: number; offset: number }[];
}

export class FeedVirtualizer<T> {
  // The window, sorted by config.compare. Raw, not deep: rows are only ever replaced
  // wholesale, never mutated, so a per-row proxy would buy nothing but cost on every
  // burst. The component reads this for its empty state.
  rows = $state.raw<ItemRow<T>[]>([]);
  // Bumped on every measured-height change so layout/range recompute.
  measureVersion = $state(0);
  viewTop = $state(0);
  viewH = $state(0);
  // Whether the view is pinned to the newest row (drives the jump chip).
  autoStick = $state(true);
  // Older rows than the window holds exist on the host.
  more = $state(false);
  // A load or an older page is in flight.
  fetching = $state(false);
  // An older page is in flight: what the top row's spinner shows.
  private fetchingOlder = $state(false);
  // The window's newest end was let go to keep to hardMax rows, so rows newer than its last
  // exist on the host: live rows past it only count toward `unseen`.
  detached = $state(false);
  // Rows that arrived below the viewport while the reader was scrolled up.
  unseen = $state(0);
  // The newest page failed to load and is being asked for again.
  private retrying = $state(false);
  top = $derived<TopState>(
    this.fetchingOlder ? "loading" : this.retrying ? "retrying" : this.more ? "idle" : "start",
  );

  display = $derived.by<DisplayRow<T>[]>(() => {
    const top: DisplayRow<T> = { kind: "top", clientKey: TOP_KEY, state: this.top };
    const bounds = this.config.boundaries?.() ?? [];
    if (bounds.length === 0) {
      return [top, ...this.rows];
    }
    return this.interleave(top, [...bounds].sort((a, b) => a.at - b.at));
  });

  private readonly config: FeedConfig<T>;
  private heights = new Map<number, number>();
  private seq = 0;
  // Session id -> divider clientKey, handed out once and never reused, so a divider keeps
  // its DOM node and its hold on the reader's place for the life of the feed.
  private dividerKeys = new Map<string, number>();
  // Reactive so the effects below first run once the container is attached: until then
  // they must not touch the display set at all (see the constructor).
  private scrollEl = $state.raw<HTMLDivElement | undefined>(undefined);
  private pending: T[] = [];
  private rafId = 0;
  private anchor: Anchor<T> | null = null;
  // key -> clientKey over `rows`, rebuilt whenever rows are replaced.
  private keys = new Map<string, number>();
  private guard = new RequestGuard();
  private inflight: "load" | "older" | null = null;
  // The newest clear epoch seen; a page from an older one is discarded.
  private epoch = 0;
  // Old rows that arrived while an older page was in flight, settled when it lands.
  private heldOld: T[] = [];
  // Rows that arrived while a load was in flight, merged into the page it returns.
  private loadBuf: T[] = [];
  // The remembered patches, oldest first, run over every item as it joins the window.
  // `n` counts every patch ever made and `at` is when it was (Date.now()).
  private patches: { apply: (item: T) => T; n: number; at: number }[] = [];
  private patchCount = 0;
  // The first patch the page in flight was asked for before: it may need that patch and
  // every later one. Null while no page is in flight.
  private patchStamp: number | null = null;
  // A load or an older page failed: the timer that asks again, and how long the next one
  // waits. While it runs nothing else asks, so a host that keeps failing is never polled.
  private retryTimer: ReturnType<typeof setTimeout> | undefined;
  private retryMs = RETRY_MS;
  // The window still holds a previous filter's rows: load() was called and no page has
  // replaced them yet.
  private staleFilter = false;
  // A detached window's reload is in flight: the reader asked for the newest row, and lands
  // on it once the page does. Until then the window stays detached and unstuck.
  private restickOnLoad = false;

  constructor(config: FeedConfig<T>) {
    this.config = config;

    // Before the DOM update: while the sizer still has its OLD height, (1) re-read the
    // anchor if the user scrolled since it was taken (the scroll event may not have
    // landed yet), and (2) move scrollTop UP now when the anchor moved up -- a trim or a
    // shrink above. Done after the DOM update instead, the shorter sizer would already
    // have clamped a scrollTop near the bottom, losing the position to compensate from.
    //
    // A pre effect runs synchronously, inside this constructor, before the consumer's
    // script has finished. So the scroll container is checked before anything reads the
    // display set; the container is attached after the consumer's script has finished.
    $effect.pre(() => {
      const el = this.scrollEl;
      if (!el || this.autoStick) {
        return;
      }
      const layout = this.layout;
      const rows = this.display;
      untrack(() => {
        const a = this.syncAnchor(el);
        const target = this.resolve(a, rows, layout);
        if (target !== null && target < el.scrollTop - 0.5) {
          el.scrollTop = target;
          this.anchor = this.mark(el.scrollTop, rows, layout);
        }
      });
    });

    // After the DOM update: pin to the newest row while stuck, else land the anchor
    // (a growth above can only be applied once the taller sizer exists). Depends on the
    // display set (a page or a new window), layout.total (re-pin after a freshly
    // measured row grows the sizer), autoStick (jumpToLatest/load) and the container
    // (the first pin once it attaches). Either way the view has settled, so this is
    // where the next older page is asked for.
    $effect(() => {
      const el = this.scrollEl;
      if (!el) {
        return;
      }
      const layout = this.layout;
      const rows = this.display;
      const stuck = this.autoStick;
      if (stuck) {
        el.scrollTop = el.scrollHeight;
        // Keep the window state coherent without waiting on the async scroll event.
        this.viewTop = el.scrollTop;
        untrack(() => this.maybeFetchOlder());
        return;
      }
      untrack(() => {
        const a = this.syncAnchor(el);
        const target = this.resolve(a, rows, layout);
        if (target !== null && Math.abs(target - el.scrollTop) >= 0.5) {
          el.scrollTop = target;
        }
        this.viewTop = el.scrollTop;
        this.anchor = this.mark(el.scrollTop, rows, layout);
        this.maybeFetchOlder();
      });
    });
  }

  private heightOf(row: DisplayRow<T>): number {
    const measured = this.heights.get(row.clientKey);
    if (measured !== undefined) {
      return measured;
    }
    switch (row.kind) {
      case "top":
        return this.config.topHeight;
      case "divider":
        return this.config.dividerHeight ?? this.config.topHeight;
      default:
        return this.config.estimate;
    }
  }

  // The window with each boundary's divider just before the first item at or after it.
  // A divider needs evidence it sits where it says: an item before it in the window, or
  // no older rows on the host -- else the rows before it may simply not be loaded yet. One
  // with nothing after it yet shows at the bottom while its broadcast runs. Boundaries
  // with no item between them would stack; only the latest of them shows.
  private interleave(top: DisplayRow<T>, bounds: Boundary[]): DisplayRow<T>[] {
    const { timeOf, dividerLabel } = this.config;
    const rows = this.rows;
    let earliest = Infinity;
    for (const r of rows) {
      earliest = Math.min(earliest, timeOf(r.item));
    }
    const out: DisplayRow<T>[] = [top];
    const place = (b: Boundary): void => {
      out.push({ kind: "divider", clientKey: this.dividerKey(b.id), at: b.at, label: dividerLabel?.(b) ?? "" });
    };
    let next = 0;
    for (const row of rows) {
      const t = timeOf(row.item);
      let slot: Boundary | undefined;
      for (; next < bounds.length && bounds[next].at <= t; next++) {
        slot = bounds[next];
      }
      if (slot && (earliest < slot.at || !this.more)) {
        place(slot);
      }
      out.push(row);
    }
    let tail: Boundary | undefined;
    for (; next < bounds.length; next++) {
      tail = bounds[next];
    }
    if (tail?.live && !this.detached) {
      place(tail);
    }
    return out;
  }

  private dividerKey(id: string): number {
    let key = this.dividerKeys.get(id);
    if (key === undefined) {
      key = -2 - this.dividerKeys.size;
      this.dividerKeys.set(id, key);
    }
    return key;
  }

  // Three screens of rows at the estimate, or config.base when that is more.
  private effBase(): number {
    return Math.max(this.config.base, Math.ceil((3 * this.viewH) / this.config.estimate));
  }

  // Never under four windows, so a tall window cannot trim straight back into a refill.
  private effHigh(): number {
    return Math.max(this.config.highWater, 4 * this.effBase());
  }

  // Cumulative row offsets + total height over the display set; recomputed when the
  // display set or any measured height changes. O(n) over the window -- cheap.
  layout = $derived.by<Layout>(() => {
    void this.measureVersion;
    const rows = this.display;
    const tops = new Array<number>(rows.length);
    let acc = 0;
    for (let i = 0; i < rows.length; i++) {
      tops[i] = acc;
      acc += this.heightOf(rows[i]);
    }
    return { tops, total: acc };
  });

  // Visible window [start, end) including overscan, from the scroll offset.
  private range = $derived.by<{ start: number; end: number }>(() => {
    const { tops } = this.layout;
    const rows = this.display;
    const n = rows.length;
    if (n === 0) {
      return { start: 0, end: 0 };
    }
    const overscan = this.config.overscan ?? 6;
    const top = this.viewTop;
    const bottom = this.viewTop + this.viewH;
    let start = n - 1;
    for (let i = 0; i < n; i++) {
      if (tops[i] + this.heightOf(rows[i]) > top) {
        start = i;
        break;
      }
    }
    let end = n;
    for (let i = start; i < n; i++) {
      if (tops[i] > bottom) {
        end = i;
        break;
      }
    }
    return { start: Math.max(0, start - overscan), end: Math.min(n, end + overscan) };
  });

  visible = $derived.by<(DisplayRow<T> & { top: number })[]>(() => {
    const r = this.range;
    const rows = this.display;
    return rows.slice(r.start, r.end).map((row, i) => ({ ...row, top: this.layout.tops[r.start + i] }));
  });

  // The rows intersecting the viewport at `scrollTop`, against `layout`. More than one
  // mark, so a trim that takes the top row still leaves the next one to hold on to. The
  // top row is never one: it stays pinned at index 0, so a page prepended under it would
  // hold it still and carry the rows the reader was looking at away.
  private mark(scrollTop: number, rows: DisplayRow<T>[], layout: Layout): Anchor<T> {
    const bottom = scrollTop + Math.max(this.viewH, 1);
    const marks: Anchor<T>["marks"] = [];
    for (let i = 0; i < rows.length; i++) {
      if (rows[i].kind === "top") {
        continue;
      }
      const top = layout.tops[i];
      const end = i + 1 < rows.length ? layout.tops[i + 1] : layout.total;
      if (end <= scrollTop) {
        continue;
      }
      if (top >= bottom && marks.length > 0) {
        break;
      }
      marks.push({ key: rows[i].clientKey, offset: scrollTop - top });
    }
    return { at: scrollTop, rows, layout, marks };
  }

  // The anchor for the element's current scrollTop. A scrollTop that no longer matches
  // the last mark is the user's own scroll, not yet reported; it was made against the
  // layout the anchor was taken on, so it is re-read there.
  private syncAnchor(el: HTMLDivElement): Anchor<T> {
    const a = this.anchor;
    if (a && Math.abs(a.at - el.scrollTop) < 0.5) {
      return a;
    }
    const next = a ? this.mark(el.scrollTop, a.rows, a.layout) : this.mark(el.scrollTop, this.display, this.layout);
    this.anchor = next;
    return next;
  }

  // Where scrollTop has to be for the anchor's first surviving row to sit where it sat;
  // null when none of the rows the reader was looking at survive.
  private resolve(a: Anchor<T>, rows: DisplayRow<T>[], layout: Layout): number | null {
    if (a.marks.length === 0) {
      return null;
    }
    const wanted = new Set(a.marks.map((m) => m.key));
    const index = new Map<number, number>();
    for (let i = 0; i < rows.length; i++) {
      if (wanted.has(rows[i].clientKey)) {
        index.set(rows[i].clientKey, i);
      }
    }
    for (const m of a.marks) {
      const i = index.get(m.key);
      if (i !== undefined) {
        return Math.max(0, layout.tops[i] + m.offset);
      }
    }
    return null;
  }

  private setInflight(kind: "load" | "older" | null): void {
    this.inflight = kind;
    // A page asked for now is read after every patch so far. Settling does not trim: the
    // page is patched after this, as it joins the window.
    this.patchStamp = kind === null ? null : this.patchCount;
    if (kind !== null) {
      this.trimPatches();
    }
    this.fetching = kind !== null;
    this.fetchingOlder = kind === "older";
  }

  // Forget the rows queued for the next frame.
  private dropPending(): void {
    if (this.rafId) {
      cancelAnimationFrame(this.rafId);
    }
    this.rafId = 0;
    this.pending = [];
  }

  private cancelRetry(): void {
    clearTimeout(this.retryTimer);
    this.retryTimer = undefined;
  }

  private setRows(rows: ItemRow<T>[]): void {
    const keyOf = this.config.key;
    this.keys = new Map(rows.map((r) => [keyOf(r.item), r.clientKey]));
    this.rows = rows;
  }

  private byOrder = (a: ItemRow<T>, b: ItemRow<T>): number => this.config.compare(a.item, b.item);

  // `items` (sorted) as the whole window, keeping the clientKey of every row whose key
  // stays, and with it its measured height and its hold on the reader's scroll position.
  private replaceRows(items: T[]): void {
    const keyOf = this.config.key;
    const rows: ItemRow<T>[] = items.map((item) => ({
      kind: "item",
      clientKey: this.keys.get(keyOf(item)) ?? ++this.seq,
      item: this.patched(item),
    }));
    const live = new Set(rows.map((r) => r.clientKey));
    for (const key of [...this.heights.keys()]) {
      if (key > 0 && !live.has(key)) {
        this.heights.delete(key);
      }
    }
    this.setRows(rows);
    this.measureVersion++;
  }

  /** Replace the window with the newest page for the current filter, which the rows it
   * holds may no longer match. Supersedes any page in flight; only a later load or a reset
   * supersedes it. Untracked, so an effect that calls it (a filter switch) does not come to
   * depend on the state it reads. */
  load = (): void => {
    this.staleFilter = true;
    this.retrying = false;
    this.restickOnLoad = false;
    this.fetchNewest();
  };

  // Ask for the newest page: under a new filter when load() says so, else under the one
  // the window's rows already match (a retry, or reloading a detached window). A new filter
  // lands on the newest row at once; a retry leaves the reader where they are.
  private fetchNewest(restick = true): void {
    untrack(() => {
      this.guard.supersede();
      const ok = this.guard.claim("load");
      this.cancelRetry();
      // Rows still queued for the next frame may have passed a filter being replaced. The
      // host stores a row before it pushes it, so the page brings back any that match.
      this.dropPending();
      this.setInflight("load");
      this.heldOld = [];
      this.loadBuf = [];
      if (restick) {
        this.autoStick = true;
        this.unseen = 0;
        this.detached = false;
      }
      this.config.fetch({ limit: this.effBase() }).then(
        (page) => {
          if (ok()) {
            this.applyLoad(page);
          }
        },
        () => {
          if (ok()) {
            this.failLoad();
          }
        },
      );
    });
  }

  // A window of another filter's rows is emptied, since none of them may stay; one that
  // already matches the filter keeps its rows. Either way what arrived while the load was
  // in flight matches and shows, and the host is asked again after a pause.
  private failLoad(): void {
    const buffered = this.loadBuf;
    this.loadBuf = [];
    this.setInflight(null);
    this.restickOnLoad = false;
    if (this.staleFilter) {
      this.staleFilter = false;
      this.more = true;
      this.detached = false;
      this.replaceRows([]);
    }
    this.release(buffered);
    this.retrying = true;
    this.retryAfterPause(() => this.fetchNewest(false));
  }

  private retryAfterPause(run: () => void): void {
    this.retryTimer = setTimeout(() => {
      this.retryTimer = undefined;
      run();
    }, this.retryMs);
    this.retryMs = Math.min(this.retryMs * 2, RETRY_MAX_MS);
  }

  // The new window is the page plus whatever arrived while it was in flight and sorts
  // after the page's oldest row, deduped by key. What sorts before it is kept only when
  // the host has nothing older: otherwise the host holds it and a later page brings it.
  private applyLoad(page: FeedPage<T>): void {
    const buffered = this.loadBuf;
    this.loadBuf = [];
    this.heldOld = [];
    this.setInflight(null);
    this.retryMs = RETRY_MS;
    this.retrying = false;
    this.staleFilter = false;
    const restick = this.restickOnLoad;
    this.restickOnLoad = false;
    if (page.epoch < this.epoch) {
      this.release(buffered);
      return;
    }
    this.epoch = page.epoch;
    const { key, compare } = this.config;
    const byKey = new Map<string, T>();
    for (const item of page.items) {
      byKey.set(key(item), item);
    }
    const oldest = page.items[0];
    for (const item of buffered) {
      const k = key(item);
      if (byKey.has(k) || (oldest !== undefined && page.more && compare(item, oldest) < 0)) {
        continue;
      }
      byKey.set(k, item);
    }
    // A detached reload the reader asked for lands on the newest row, and so does a window
    // where nothing the reader could have scrolled to survives (a filter switch).
    if (restick || ![...byKey.keys()].some((k) => this.keys.has(k))) {
      this.autoStick = true;
      this.unseen = 0;
    }
    this.more = page.more;
    this.detached = false;
    this.replaceRows([...byKey.values()].sort(compare));
  }

  // Asks for the page before the window's oldest row once the reader is near the top of
  // the window. Called from the scroll handler and after every settle of the view.
  private maybeFetchOlder(): void {
    if (
      this.inflight !== null ||
      this.retryTimer !== undefined ||
      !this.more ||
      this.rows.length === 0
    ) {
      return;
    }
    if (!this.scrollEl || this.viewTop >= (this.config.prefetchScreens ?? 1.5) * this.viewH) {
      return;
    }
    const ok = this.guard.claim("older");
    this.setInflight("older");
    const cursor = this.rows[0].item;
    const cursorKey = this.config.key(cursor);
    this.config.fetch({ before: cursor, limit: this.config.page }).then(
      (page) => {
        if (ok()) {
          this.applyOlder(page, cursorKey);
        }
      },
      () => {
        // The host could not answer yet (chat history still opening, say): the older rows
        // are still there, so the top stays open and the page is asked for again.
        if (ok()) {
          this.setInflight(null);
          this.heldOld = [];
          this.retryAfterPause(() => this.maybeFetchOlder());
        }
      },
    );
  }

  // Prepends an older page, but only onto the window it was asked for: from the current
  // epoch, and with the cursor row still the window's oldest.
  private applyOlder(page: FeedPage<T>, cursorKey: string): void {
    const held = this.heldOld;
    this.heldOld = [];
    this.setInflight(null);
    this.retryMs = RETRY_MS;
    const { key, compare } = this.config;
    if (page.epoch < this.epoch || this.rows.length === 0 || key(this.rows[0].item) !== cursorKey) {
      return;
    }
    const known = new Set(this.keys.keys());
    const added: ItemRow<T>[] = [];
    for (const item of page.items) {
      const k = key(item);
      if (!known.has(k)) {
        known.add(k);
        added.push(this.newRow(item));
      }
    }
    // An old row that arrived meanwhile: kept if it sorts inside the new window, or if the
    // host has nothing older; otherwise the next page brings it.
    const newOldest = page.items[0] ?? this.rows[0].item;
    let sort = false;
    for (const item of held) {
      const k = key(item);
      if (known.has(k) || (page.more && compare(item, newOldest) < 0)) {
        continue;
      }
      known.add(k);
      added.push(this.newRow(item));
      sort = true;
    }
    const next = added.concat(this.rows);
    if (sort) {
      next.sort(this.byOrder);
    }
    this.more = page.more;
    this.setRows(this.capWindow(next));
  }

  // Scrolled up, the window is capped to hardMax rows. The excess goes from the newest end
  // first, below the reader, and the window detaches; whatever that cannot cover goes from
  // the oldest end, above the reader. A row the viewport shows is never let go, so the rows
  // the reader sees keep their place, and the window stays over the cap when they leave too
  // few rows outside them. An older page or a live row lands at an end, so the newest end
  // covers it; a backfill that sorts into the middle of a full window may need both.
  private capWindow(next: ItemRow<T>[]): ItemRow<T>[] {
    const max = this.config.hardMax;
    if (this.autoStick || next.length <= max) {
      return next;
    }
    const seen = new Set(this.visibleRows().map((r) => r.clientKey));
    let first = -1;
    let last = -1;
    next.forEach((r, i) => {
      if (seen.has(r.clientKey)) {
        first = first < 0 ? i : first;
        last = i;
      }
    });
    const excess = next.length - max;
    const fromNewest = Math.min(excess, last < 0 ? excess : next.length - 1 - last);
    const fromOldest = Math.min(excess - fromNewest, first < 0 ? 0 : first);
    let out = next;
    if (fromNewest > 0) {
      for (const r of out.slice(out.length - fromNewest)) {
        this.heights.delete(r.clientKey);
      }
      out = out.slice(0, out.length - fromNewest);
      this.detached = true;
    }
    return fromOldest > 0 ? this.trimOldest(out, fromOldest) : out;
  }

  // Let the window's `cut` oldest rows go. The host still holds them, so there is more to
  // page again; an older page in flight would land above the gap they leave, so it is
  // discarded.
  private trimOldest(rows: ItemRow<T>[], cut: number): ItemRow<T>[] {
    for (const r of rows.slice(0, cut)) {
      this.heights.delete(r.clientKey);
    }
    this.more = true;
    this.guard.claim("older");
    if (this.inflight === "older") {
      this.setInflight(null);
    }
    this.heldOld = [];
    return rows.slice(cut);
  }

  // `item` with every remembered patch run over it.
  private patched(item: T): T {
    let out = item;
    for (const p of this.patches) {
      out = p.apply(out);
    }
    return out;
  }

  // Forget the patches nothing can need any more: older than the page in flight and past
  // PATCH_RETAIN_MS. Past PATCH_MEMORY_MAX the oldest go whatever they are.
  private trimPatches(): void {
    const now = Date.now();
    const stamp = this.patchStamp ?? Infinity;
    let drop = 0;
    while (drop < this.patches.length) {
      const p = this.patches[drop];
      const needed = p.n >= stamp || now - p.at < PATCH_RETAIN_MS;
      if (needed && this.patches.length - drop <= PATCH_MEMORY_MAX) {
        break;
      }
      drop++;
    }
    if (drop > 0) {
      this.patches = this.patches.slice(drop);
    }
  }

  // A row for an item new to the window.
  private newRow(item: T): ItemRow<T> {
    return { kind: "item", clientKey: ++this.seq, item: this.patched(item) };
  }

  /** Replace every item `match` picks with `update(item)`, which must keep its key (a
   * moderation op redacting chat rows, say). A row keeps its clientKey and its place, so
   * its measured height stays until the row re-measures and the reader's place is held
   * like any other height change. An update returning the item itself changes nothing.
   *
   * The patch is also remembered (while a page asked for before it is in flight, and for
   * PATCH_RETAIN_MS, up to PATCH_MEMORY_MAX of them; a clear forgets them all) and run over
   * every item that joins the window later: a live row queued or posted late, a page read
   * before it. So `match` must pick only items that existed when it was made -- a
   * moderation op names rows the host admitted before it, never one admitted after -- and
   * should be cheap, since every item joining the window meets every remembered patch. */
  patch = (match: (item: T) => boolean, update: (item: T) => T): void => {
    const apply = (item: T): T => (match(item) ? update(item) : item);
    this.patches.push({ apply, n: this.patchCount++, at: Date.now() });
    this.trimPatches();
    let changed = false;
    const rows = this.rows.map((r) => {
      const item = apply(r.item);
      if (item === r.item) {
        return r;
      }
      changed = true;
      return { ...r, item };
    });
    if (changed) {
      this.rows = rows;
    }
  };

  /** A live row (events.new, chat.message) that matches the current filter. */
  live = (item: T): void => {
    this.pending.push(item);
    this.schedule();
  };

  /** A batch that may hold rows already shown (events.backfill): each joins at its place
   * in the order, and one already in the window is skipped. */
  merge = (items: readonly T[]): void => {
    this.pending.push(...items);
    this.schedule();
  };

  // Batch incoming rows onto a single rAF flush so a burst re-renders once (not once
  // per row) and the array is rebuilt at most once per frame.
  private schedule(): void {
    if (!this.rafId) {
      this.rafId = requestAnimationFrame(this.flush);
    }
  }

  private flush = (): void => {
    this.rafId = 0;
    const batch = this.pending;
    this.pending = [];
    if (this.inflight === "load") {
      this.loadBuf.push(...batch);
      return;
    }
    this.release(batch);
  };

  // Places arrived rows into the window. A row older than the window's oldest is the
  // host's to page in, unless the host has nothing older; while an older page is in
  // flight it waits for that page, which may cover it.
  private release(batch: T[]): void {
    const { key, compare } = this.config;
    const oldest = this.rows[0]?.item;
    const newest = this.rows[this.rows.length - 1]?.item;
    const seen = new Set<string>();
    const accepted: T[] = [];
    // Rows past a detached window's newest end, which it let go of: they only count toward
    // `unseen`. One that sorts inside the window still joins it.
    const missed: T[] = [];
    for (const item of batch) {
      const k = key(item);
      if (this.keys.has(k) || seen.has(k)) {
        continue;
      }
      seen.add(k);
      if (oldest !== undefined && compare(item, oldest) < 0 && this.more) {
        if (this.inflight === "older") {
          this.heldOld.push(item);
        }
        continue;
      }
      if (this.detached && newest !== undefined && compare(item, newest) > 0) {
        missed.push(item);
        continue;
      }
      accepted.push(item);
    }
    if (!this.autoStick) {
      this.unseen += this.countBelow(missed.concat(accepted));
    }
    if (accepted.length === 0) {
      return;
    }
    const added: ItemRow<T>[] = accepted.map((item) => this.newRow(item));
    let next = this.rows.concat(added);
    const lastBefore = this.rows[this.rows.length - 1];
    if (lastBefore !== undefined && added.some((r) => this.byOrder(r, lastBefore) < 0)) {
      next.sort(this.byOrder);
    } else if (added.length > 1) {
      next = this.rows.concat(added.sort(this.byOrder));
    }
    if (this.autoStick && next.length > this.effHigh()) {
      // Trim back to base. Only an older page in flight is invalidated, never a load, which
      // rebuilds the window itself.
      next = this.trimOldest(next, next.length - this.effBase());
    }
    this.setRows(this.capWindow(next));
  }

  // How many of `items` sort after the newest row the viewport shows: the ones the reader
  // has not seen. A row that joins above the viewport is not one.
  private countBelow(items: T[]): number {
    const last = this.lastVisible();
    const compare = this.config.compare;
    return last === undefined ? items.length : items.filter((i) => compare(i, last) > 0).length;
  }

  // The item rows the viewport shows, oldest first.
  private visibleRows(): ItemRow<T>[] {
    const rows = this.display;
    const { tops, total } = this.layout;
    const bottom = this.viewTop + this.viewH;
    const out: ItemRow<T>[] = [];
    for (let i = 0; i < rows.length && tops[i] < bottom; i++) {
      const row = rows[i];
      const end = i + 1 < rows.length ? tops[i + 1] : total;
      if (row.kind === "item" && end > this.viewTop) {
        out.push(row);
      }
    }
    return out;
  }

  // The newest item the viewport shows, or undefined when it shows none.
  private lastVisible(): T | undefined {
    const rows = this.display;
    const { tops } = this.layout;
    const bottom = this.viewTop + this.viewH;
    let last: T | undefined;
    for (let i = 0; i < rows.length && tops[i] < bottom; i++) {
      const row = rows[i];
      if (row.kind === "item") {
        last = row.item;
      }
    }
    return last;
  }

  /** The store was cleared at `epoch`: drop every row. A repeat of an epoch already
   * applied (the clear call's reply after its push, or a page read after the clear) is
   * ignored, so it cannot drop rows that arrived since. */
  reset(epoch: number): void {
    if (epoch <= this.epoch) {
      return;
    }
    this.guard.supersede();
    this.epoch = epoch;
    this.patches = [];
    this.cancelRetry();
    this.retryMs = RETRY_MS;
    this.retrying = false;
    this.staleFilter = false;
    this.restickOnLoad = false;
    this.dropPending();
    this.heldOld = [];
    this.loadBuf = [];
    this.setInflight(null);
    this.more = false;
    this.detached = false;
    this.unseen = 0;
    this.autoStick = true;
    this.replaceRows([]);
  }

  /** Back to the newest row; a detached window is reloaded, since its tail is stale. */
  jumpToLatest = (): void => {
    if (this.detached) {
      this.reloadDetached();
      return;
    }
    this.autoStick = true;
    this.unseen = 0;
    if (this.scrollEl) {
      this.scrollEl.scrollTop = this.scrollEl.scrollHeight;
    }
  };

  // A detached window's tail is stale, so the newest row is reached by reloading. It stays
  // detached, and so unstuck, with its chip and unseen count, until the page lands.
  private reloadDetached(): void {
    this.restickOnLoad = true;
    this.fetchNewest(false);
  }

  // Action for the scroll container: tracks the offset/viewport, the stuck flag and the
  // scroll anchor, and asks for older rows as the reader nears the window's top.
  scroll = (node: HTMLDivElement): { destroy(): void } => {
    this.scrollEl = node;
    this.viewH = node.clientHeight;
    const stickPx = this.config.stickPx ?? 24;
    const onScroll = (): void => {
      this.viewTop = node.scrollTop;
      this.viewH = node.clientHeight;
      const atBottom = node.scrollHeight - node.scrollTop - node.clientHeight <= stickPx;
      this.anchor = this.mark(node.scrollTop, this.display, this.layout);
      if (this.detached) {
        // The detached tail is stale, so reaching it reloads rather than sticking to it;
        // the older end pages on as ever.
        if (atBottom && this.inflight !== "load") {
          this.reloadDetached();
        } else {
          this.maybeFetchOlder();
        }
        return;
      }
      this.autoStick = atBottom;
      if (atBottom) {
        this.unseen = 0;
      }
      this.maybeFetchOlder();
    };
    node.addEventListener("scroll", onScroll);
    const ro = new ResizeObserver(() => {
      this.viewH = node.clientHeight;
      this.maybeFetchOlder();
    });
    ro.observe(node);
    return {
      destroy: () => {
        node.removeEventListener("scroll", onScroll);
        ro.disconnect();
        if (this.scrollEl === node) {
          this.scrollEl = undefined;
          this.anchor = null;
        }
      },
    };
  };

  // Action per rendered row: measure its real (wrapped/emote) height; a delta bumps
  // measureVersion so the layout, the bottom pin and the scroll anchor reflect it.
  measureRow = (node: HTMLElement, key: number): { destroy(): void } => {
    const apply = (): void => {
      const h = node.offsetHeight;
      if (h > 0 && this.heights.get(key) !== h) {
        this.heights.set(key, h);
        this.measureVersion++;
      }
    };
    const ro = new ResizeObserver(apply);
    ro.observe(node);
    apply();
    return { destroy: () => ro.disconnect() };
  };

  // Cancel any in-flight rAF and load retry, drop pending rows, and discard any page still
  // in flight (call from the consumer's teardown). The scroll/measureRow actions clean up
  // their own listeners.
  dispose(): void {
    this.dropPending();
    this.cancelRetry();
    this.guard.supersede();
    this.setInflight(null);
  }
}
