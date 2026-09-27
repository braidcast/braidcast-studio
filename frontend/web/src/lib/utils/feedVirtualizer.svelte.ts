// Shared virtualized-feed engine for the Multichat and Events docks. Both render a
// ring-capped, bottom-sticky, absolutely-positioned virtual list measured row by
// row; this extracts the identical machinery (rAF-batched enqueue, ring trim,
// per-row measurement, visible-range windowing, auto-stick-to-bottom, scroll
// anchoring) so the two docks share one implementation.
//
// The docks diverge in a few ways, all parameterized here rather than picked:
//   - row-height ESTIMATE (chat 30px, events 38px)      -> config.estimate
//   - ring cap MAX (chat 1000, events 500)              -> config.max
//   - each shows a derived subset of its ring           -> config.getDisplay
//   - item identity across a whole-feed replace         -> config.key
// Both whole-feed-replace through setFeed (events on list/backfill/clear, chat once
// when it hydrates) and append through enqueue.
//
// A consumer constructs one instance in its <script> (so the internal effects bind
// to the component), renders `visible` over a `layout.total`-high sizer, and wires
// the two actions (`scroll` on the scroll container, `measureRow` per row). The
// display set is whatever getDisplay returns (the full ring by default, or a
// filtered/sorted subset), so height keys stay stable across filtering.
//
// Scroll anchoring: rows sit at `style:top` over a sizer, which suppresses CSS scroll
// anchoring, so this does it by hand. While the reader is scrolled up, every layout
// change (a trim, a row measured, a row inserted above, a whole-feed replace, a display
// change) puts the row under the reader's eye back at the same viewport offset.

import { untrack } from "svelte";

export interface FeedRow<T> {
  clientKey: number;
  item: T;
}

interface FeedConfig<T> {
  /** Hard cap on retained rows; the oldest are trimmed (heights pruned in lockstep). */
  max: number;
  /** Px height estimate for an unmeasured row. */
  estimate: number;
  /** Rows rendered beyond the viewport on each side (default 6). */
  overscan?: number;
  /** Within this many px of the bottom counts as "stuck to latest" (default 24). */
  stickPx?: number;
  /** The rows to display — the full ring by default, or a filtered subset. */
  getDisplay?: () => FeedRow<T>[];
  /** Stable identity for an item. With it, setFeed keeps a surviving item's clientKey,
   * and with the key its measured height and its place as the reader's scroll anchor;
   * without it a whole-feed replace starts every row fresh. */
  key?: (item: T) => string;
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
  rows: FeedRow<T>[];
  layout: Layout;
  marks: { key: number; offset: number }[];
}

export class FeedVirtualizer<T> {
  // The retained ring (append order, oldest -> newest). Raw, not deep: rows are only
  // ever replaced wholesale (flush/setFeed assign a fresh array), never mutated, so a
  // per-row proxy would buy nothing but cost on every burst. The component reads this
  // for its empty-state and derives any filtered display set from it.
  rows = $state.raw<FeedRow<T>[]>([]);
  // Bumped on every measured-height change so layout/range recompute.
  measureVersion = $state(0);
  viewTop = $state(0);
  viewH = $state(0);
  // Whether the view is pinned to the newest row (drives the jump-to-latest chip).
  autoStick = $state(true);

  private readonly config: FeedConfig<T>;
  private heights = new Map<number, number>();
  private seq = 0;
  // Reactive so the effects below first run once the container is attached: until then
  // they must not touch the display set at all (see the constructor).
  private scrollEl = $state.raw<HTMLDivElement | undefined>(undefined);
  private pending: T[] = [];
  private rafId = 0;
  private anchor: Anchor<T> | null = null;

  constructor(config: FeedConfig<T>) {
    this.config = config;

    // Before the DOM update: while the sizer still has its OLD height, (1) re-read the
    // anchor if the user scrolled since it was taken (the scroll event may not have
    // landed yet), and (2) move scrollTop UP now when the anchor moved up -- a trim or a
    // shrink above. Done after the DOM update instead, the shorter sizer would already
    // have clamped a scrollTop near the bottom, losing the position to compensate from.
    //
    // A pre effect runs synchronously, inside this constructor, and the consumer declares
    // the set getDisplay reads only after constructing the feed (it derives from
    // feed.rows). So the scroll container is checked before anything reads the display
    // set; the container is attached after the consumer's script has finished.
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
    // display set (re-pin on a filter switch), layout.total (re-pin after a freshly
    // measured row grows the sizer), autoStick (jumpToLatest/restick) and the container
    // (the first pin once it attaches).
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
      });
    });
  }

  private get display(): FeedRow<T>[] {
    return this.config.getDisplay ? this.config.getDisplay() : this.rows;
  }

  private heightOf(row: FeedRow<T>): number {
    return this.heights.get(row.clientKey) ?? this.config.estimate;
  }

  // Cumulative row offsets + total height over the display set; recomputed when the
  // display set or any measured height changes. O(n) over the <=MAX ring -- cheap.
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

  visible = $derived.by<(FeedRow<T> & { top: number })[]>(() => {
    const r = this.range;
    const rows = this.display;
    return rows.slice(r.start, r.end).map((row, i) => ({ ...row, top: this.layout.tops[r.start + i] }));
  });

  // The rows intersecting the viewport at `scrollTop`, against `layout`. More than one
  // mark, so a trim that takes the top row still leaves the next one to hold on to.
  private mark(scrollTop: number, rows: FeedRow<T>[], layout: Layout): Anchor<T> {
    const bottom = scrollTop + Math.max(this.viewH, 1);
    const marks: Anchor<T>["marks"] = [];
    for (let i = 0; i < rows.length; i++) {
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
  private resolve(a: Anchor<T>, rows: FeedRow<T>[], layout: Layout): number | null {
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

  // Batch incoming rows onto a single rAF flush so a burst re-renders once (not once
  // per row) and the array is rebuilt at most once per frame.
  enqueue = (item: T): void => {
    this.pending.push(item);
    if (!this.rafId) {
      this.rafId = requestAnimationFrame(this.flush);
    }
  };

  private flush = (): void => {
    this.rafId = 0;
    if (this.pending.length === 0) {
      return;
    }
    let next = this.rows.concat(this.pending.map((item) => ({ clientKey: ++this.seq, item })));
    this.pending = [];
    if (next.length > this.config.max) {
      // clientKeys are unique+monotonic, so trimmed rows never alias a kept one --
      // prune their heights directly, in lockstep with the ring trim.
      for (const d of next.slice(0, next.length - this.config.max)) {
        this.heights.delete(d.clientKey);
      }
      next = next.slice(next.length - this.config.max);
    }
    this.rows = next;
  };

  // Replace the whole feed (events list/backfill/clear, chat hydrate). `reverse` flips a
  // newest-first source into oldest->newest (top->bottom) append order. Drops any
  // pending appends. With config.key, an item already shown keeps its clientKey (and so
  // its measured height and its hold on the reader's scroll position); every other row
  // starts fresh. The stuck state is left alone: a reader scrolled up stays where they
  // are, and one at the bottom stays pinned there.
  setFeed(list: T[], reverse = false): void {
    this.pending = [];
    if (this.rafId) {
      cancelAnimationFrame(this.rafId);
      this.rafId = 0;
    }
    const keyOf = this.config.key;
    const kept = new Map<string, number>();
    if (keyOf) {
      for (const r of this.rows) {
        kept.set(keyOf(r.item), r.clientKey);
      }
    }
    const n = Math.min(list.length, this.config.max);
    const rows: FeedRow<T>[] = new Array(n);
    const live = new Set<number>();
    for (let i = 0; i < n; i++) {
      const item = reverse ? list[n - 1 - i] : list[list.length - n + i];
      const k = keyOf ? keyOf(item) : undefined;
      let clientKey = k !== undefined ? kept.get(k) : undefined;
      if (clientKey === undefined || live.has(clientKey)) {
        clientKey = ++this.seq;
      }
      live.add(clientKey);
      rows[i] = { clientKey, item };
    }
    for (const key of [...this.heights.keys()]) {
      if (!live.has(key)) {
        this.heights.delete(key);
      }
    }
    this.rows = rows;
    this.measureVersion++;
    if (rows.length === 0) {
      // Nothing left to be scrolled up in.
      this.autoStick = true;
    }
  }

  jumpToLatest = (): void => {
    this.autoStick = true;
    if (this.scrollEl) {
      this.scrollEl.scrollTop = this.scrollEl.scrollHeight;
    }
  };

  // Called by the consumer whenever it flips the display filter, so the view re-pins
  // to the newest of the new subset.
  restick(): void {
    this.autoStick = true;
  }

  // Action for the scroll container: tracks the offset/viewport, the stuck flag and the
  // scroll anchor.
  scroll = (node: HTMLDivElement): { destroy(): void } => {
    this.scrollEl = node;
    this.viewH = node.clientHeight;
    const stickPx = this.config.stickPx ?? 24;
    const onScroll = (): void => {
      this.viewTop = node.scrollTop;
      this.viewH = node.clientHeight;
      this.autoStick = node.scrollHeight - node.scrollTop - node.clientHeight <= stickPx;
      this.anchor = this.mark(node.scrollTop, this.display, this.layout);
    };
    node.addEventListener("scroll", onScroll);
    const ro = new ResizeObserver(() => (this.viewH = node.clientHeight));
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

  // Cancel any in-flight rAF + drop pending appends (call from the consumer's
  // teardown). The scroll/measureRow actions clean up their own listeners.
  dispose(): void {
    if (this.rafId) {
      cancelAnimationFrame(this.rafId);
    }
    this.rafId = 0;
    this.pending = [];
  }
}
