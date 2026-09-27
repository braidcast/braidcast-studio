// Which rects this window cuts out of its native preview surfaces, when each one may join
// or leave the cut, and whether the surfaces' pointer input is held for an open layer --
// kept free of runes and the DOM so the ordering can be tested. The store in
// $lib/stores/previewCutouts wires it to the bridge, the observers and the page.

/** A viewport rect in CSS px, as preview.setCutouts takes it. */
export interface CutRect {
  x: number;
  y: number;
  w: number;
  h: number;
}

/** The overlap of two rects, or null when they do not overlap. */
export function intersectCut(a: CutRect, b: CutRect): CutRect | null {
  const x = Math.max(a.x, b.x);
  const y = Math.max(a.y, b.y);
  const right = Math.min(a.x + a.w, b.x + b.w);
  const bottom = Math.min(a.y + a.h, b.y + b.h);
  return right > x && bottom > y ? { x, y, w: right - x, h: bottom - y } : null;
}

export function sameCut(a: CutRect | null, b: CutRect | null): boolean {
  return a === b || (!!a && !!b && a.x === b.x && a.y === b.y && a.w === b.w && a.h === b.h);
}

/** Everything the host holds for this window: the holes, and whether a layer is open. */
export interface CutoutState {
  rects: CutRect[];
  /** Any layer is open, cut yet or not: the surfaces' pointer input belongs to it. */
  grab: boolean;
}

export interface CutoutDeps {
  /**
   * Hand the host the window's whole state. Resolves once it has applied it, with whether each
   * rect overlaps one of the window's surfaces, or null when the call failed. Never rejects.
   */
  send(state: CutoutState): Promise<boolean[] | null>;
  /** Resolves once a frame painted after the call has been presented, or a bound ran out. */
  presented(current: () => boolean): Promise<void>;
  /** Resolves when a failed send should be tried again. */
  retryDelay(): Promise<void>;
}

/** How many times one state is sent before the set stops trying and lets its waiters go. */
export const SEND_ATTEMPTS = 4;

/** One floating layer's part of the cut. */
export interface CutoutLayer {
  /** The rect this layer has cut, null until its first presentation. */
  cut: CutRect | null;
  /**
   * Whether the host found `answeredCut` over one of its surfaces; null while a send carrying
   * a different rect is unanswered, or failed, since the host may hold that one instead.
   */
  overSurface: boolean | null;
  /** The rect `overSurface` answers for. */
  answeredCut: CutRect | null;
  /** The last send carrying this layer's rect. */
  sentIn: number;
  /** Bumped by every reshape and by the release, so a wait for an older shape lands nowhere. */
  shape: number;
  released: Promise<void> | null;
}

// What the host holds before this page has sent anything, or after it gave up.
const NOTHING_SENT = "";

export class CutoutSet {
  readonly #layers = new Set<CutoutLayer>();
  readonly #deps: CutoutDeps;
  #sentKey = NOTHING_SENT;
  #lastSend: Promise<void> = Promise.resolve();
  #sends = 0;

  constructor(deps: CutoutDeps) {
    this.#deps = deps;
  }

  /** A layer opened: the grab starts now, its rect joins once presented (reshape). */
  add(): CutoutLayer {
    const layer: CutoutLayer = { cut: null, overSurface: null, answeredCut: null, sentIn: 0, shape: 0, released: null };
    this.#layers.add(layer);
    void this.#send();
    return layer;
  }

  /**
   * The layer now measures `next` (null when it has no box). The part of its cut it no longer
   * covers is given back at once, since the page stops painting there with the frame that
   * shrank it; the part it newly covers is cut once that frame has been presented, since
   * cutting sooner would show the page from before it. Resolves when this shape has landed
   * or been overtaken.
   */
  async reshape(layer: CutoutLayer, next: CutRect | null): Promise<void> {
    if (layer.released) {
      return;
    }
    const shape = ++layer.shape;
    const kept = layer.cut && next ? intersectCut(layer.cut, next) : null;
    if (!sameCut(kept, layer.cut)) {
      layer.cut = kept;
      void this.#send();
    }
    if (!next || sameCut(next, layer.cut)) {
      return;
    }
    const current = () => layer.shape === shape && !layer.released;
    await this.#deps.presented(current);
    if (current()) {
      layer.cut = next;
      await this.#send();
    }
  }

  /**
   * Give the layer's rect back; resolves once the host has closed its hole, or once the set
   * has stopped trying (SEND_ATTEMPTS). Idempotent.
   */
  release(layer: CutoutLayer): Promise<void> {
    if (!layer.released) {
      layer.shape++;
      this.#layers.delete(layer);
      layer.released = this.#send();
    }
    return layer.released;
  }

  /** Send the whole state again, whether or not it changed: the scale it was converted at did, or the host forgot it. */
  resend(): Promise<void> {
    this.#sentKey = NOTHING_SENT;
    return this.#send();
  }

  // The state to send, with the layer behind each of its rects.
  #state(): { state: CutoutState; cutters: CutoutLayer[] } {
    const cutters = [...this.#layers].filter((l) => l.cut);
    return { state: { rects: cutters.map((l) => l.cut!), grab: this.#layers.size > 0 }, cutters };
  }

  // The last state sent is the one standing: the host handles bridge calls in order.
  #send(): Promise<void> {
    const next = this.#state();
    const key = JSON.stringify(next.state);
    if (key !== this.#sentKey) {
      this.#sentKey = key;
      this.#lastSend = this.#deliver(next, key, 1);
    }
    return this.#lastSend;
  }

  // Resolves once `state`, or a state sent after it, has landed -- or the attempts ran out.
  async #deliver(
    { state, cutters }: { state: CutoutState; cutters: CutoutLayer[] },
    key: string,
    attempt: number,
  ): Promise<void> {
    const send = ++this.#sends;
    for (const layer of cutters) {
      layer.sentIn = send;
      // The same rect again -- another layer's change resent the set -- keeps its answer.
      if (!sameCut(layer.cut, layer.answeredCut)) {
        layer.overSurface = null;
      }
    }
    const hits = await this.#deps.send(state);
    if (hits) {
      cutters.forEach((layer, i) => {
        if (layer.sentIn === send) {
          // A reply short of a rect is read as a hole: the likeness is the safe side.
          layer.overSurface = hits[i] ?? true;
          layer.answeredCut = state.rects[i];
        }
      });
      return;
    }
    if (this.#sentKey !== key) {
      // Overtaken: the later send stands for this one.
      return this.#lastSend;
    }
    // The host did not apply it, so what it holds is unknown; until a send lands, no state
    // may be skipped as already sent.
    this.#sentKey = NOTHING_SENT;
    if (attempt >= SEND_ATTEMPTS) {
      return;
    }
    await this.#deps.retryDelay();
    if (this.#sentKey !== NOTHING_SENT) {
      return this.#lastSend;
    }
    const next = this.#state();
    const nextKey = JSON.stringify(next.state);
    this.#sentKey = nextKey;
    this.#lastSend = this.#deliver(next, nextKey, attempt + 1);
    return this.#lastSend;
  }
}

/**
 * Whether the web view may show through where `layer` was: it has a hole the host has not been
 * told it lost, unless the host reported that hole off every surface.
 */
export function mayShowThrough(layer: CutoutLayer): boolean {
  return layer.cut !== null && layer.overSurface !== false;
}

/**
 * Keep a leaving layer's likeness on screen until the host has closed its hole: `restored`
 * settles, or `boundMs` runs out so a host that never answers cannot leave it up for good.
 * Then `uncover` takes the likeness away.
 */
export function holdUntilRestored(restored: Promise<void>, uncover: () => void, boundMs: number): Promise<void> {
  return new Promise((resolve) => {
    let held = true;
    const done = () => {
      if (held) {
        held = false;
        clearTimeout(timer);
        uncover();
        resolve();
      }
    };
    const timer = setTimeout(done, boundMs);
    restored.then(done, done);
  });
}
