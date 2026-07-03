// Shared virtualized-feed engine for the Multichat and Events docks. Both render a
// ring-capped, bottom-sticky, absolutely-positioned virtual list measured row by
// row; this extracts the identical machinery (rAF-batched enqueue, ring trim,
// per-row measurement, visible-range windowing, auto-stick-to-bottom) so the two
// docks share one implementation.
//
// The docks diverged in three ways, all parameterized here rather than picked:
//   - row-height ESTIMATE (chat 30px, events 38px)      -> config.estimate
//   - ring cap MAX (both 500, kept a knob)              -> config.max
//   - events filters its ring to a derived subset       -> config.getDisplay
// Events also whole-feed-replaces (list/backfill) and clears; chat only appends.
// setFeed covers both (chat simply never calls it).
//
// A consumer constructs one instance in its <script> (so the internal $effect binds
// to the component), renders `visible` over a `layout.total`-high sizer, and wires
// the two actions (`scroll` on the scroll container, `measureRow` per row). The
// display set is whatever getDisplay returns (the full ring by default, or a
// filtered subset), so height keys stay stable across filtering.

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
}

export class FeedVirtualizer<T> {
  // The retained ring (append order, oldest -> newest). The component reads this
  // for its empty-state and derives any filtered display set from it.
  rows = $state<FeedRow<T>[]>([]);
  // Bumped on every measured-height change so layout/range recompute.
  measureVersion = $state(0);
  viewTop = $state(0);
  viewH = $state(0);
  // Whether the view is pinned to the newest row (drives the jump-to-latest chip).
  autoStick = $state(true);

  private readonly config: FeedConfig<T>;
  private heights = new Map<number, number>();
  private seq = 0;
  private scrollEl: HTMLDivElement | undefined;
  private pending: T[] = [];
  private rafId = 0;

  constructor(config: FeedConfig<T>) {
    this.config = config;

    // Keep the view pinned to the newest row while stuck to the bottom. Depends on
    // the display set (re-pin on a filter switch) and layout.total (re-pin after a
    // freshly measured row grows the sizer).
    $effect(() => {
      void this.display;
      void this.layout.total;
      if (this.autoStick && this.scrollEl) {
        this.scrollEl.scrollTop = this.scrollEl.scrollHeight;
        // Keep the window state coherent without waiting on the async scroll event.
        this.viewTop = this.scrollEl.scrollTop;
      }
    });
  }

  private get display(): FeedRow<T>[] {
    return this.config.getDisplay ? this.config.getDisplay() : this.rows;
  }

  // Cumulative row offsets + total height over the display set; recomputed when the
  // display set or any measured height changes. O(n) over the <=MAX ring -- cheap.
  layout = $derived.by<{ tops: number[]; total: number }>(() => {
    void this.measureVersion;
    const rows = this.display;
    const tops = new Array<number>(rows.length);
    let acc = 0;
    for (let i = 0; i < rows.length; i++) {
      tops[i] = acc;
      acc += this.heights.get(rows[i].clientKey) ?? this.config.estimate;
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
      const h = this.heights.get(rows[i].clientKey) ?? this.config.estimate;
      if (tops[i] + h > top) {
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

  // Replace the whole feed (events list/backfill/clear). `reverse` flips a
  // newest-first source into oldest->newest (top->bottom) append order. Drops any
  // pending appends and clears measured heights (fresh clientKeys), then re-sticks.
  setFeed(list: T[], reverse = false): void {
    this.pending = [];
    if (this.rafId) {
      cancelAnimationFrame(this.rafId);
      this.rafId = 0;
    }
    this.heights.clear();
    const rows: FeedRow<T>[] = new Array(list.length);
    for (let i = 0; i < list.length; i++) {
      rows[i] = { clientKey: ++this.seq, item: reverse ? list[list.length - 1 - i] : list[i] };
    }
    this.rows = rows;
    this.measureVersion++;
    this.autoStick = true;
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

  // Action for the scroll container: tracks the offset/viewport and the stuck flag.
  scroll = (node: HTMLDivElement): { destroy(): void } => {
    this.scrollEl = node;
    this.viewH = node.clientHeight;
    const stickPx = this.config.stickPx ?? 24;
    const onScroll = (): void => {
      this.viewTop = node.scrollTop;
      this.viewH = node.clientHeight;
      this.autoStick = node.scrollHeight - node.scrollTop - node.clientHeight <= stickPx;
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
        }
      },
    };
  };

  // Action per rendered row: measure its real (wrapped/emote) height; a delta bumps
  // measureVersion so the layout + bottom anchor reflect it.
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
