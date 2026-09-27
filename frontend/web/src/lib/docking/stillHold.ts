// Which draw of a held still may reach the screen, kept free of runes and the bridge so the
// ordering can be tested. PreviewFreeze ($lib/stores/previewFreeze) supplies the draw --
// the capture and its painting -- and the reactive `frame`.

import { sameSurfaceSize, type FrozenFrame } from "$lib/docking/freezeFrame";
import type { OverlayRect } from "$lib/utils/overlayRect";

export abstract class StillHold<Target> {
  /** The held frame, or null when the surface is live. */
  abstract frame: FrozenFrame | null;

  /**
   * Draw `target` at `element` and put the result on screen as `frame`, provided `current()`
   * still holds when it arrives. Resolves once it is on screen, overtaken, or failed. Never
   * rejects.
   */
  protected abstract draw(target: Target, element: OverlayRect, current: () => boolean): Promise<void>;

  // Per-operation token. Capture and the release hold are both async, so each one checks it
  // is still the latest before touching `frame`: a capture overtaken by the modal closing
  // must not paint afterwards, and a hold overtaken by a new capture must not drop the new
  // still.
  #seq = 0;
  #releaseTimer: ReturnType<typeof setTimeout> | undefined;
  // The surface the held frame stands in for; unset once the surface is live again, so a
  // resize after that draws nothing.
  #target: Target | undefined;
  // The size the next redraw is for, and the redraws' run, so one is in flight at a time and
  // the latest size wins.
  #nextSize: OverlayRect | null = null;
  #redraws: Promise<void> | null = null;
  readonly #holdMs: number;

  /** `holdMs`: how long the still outlives release(). */
  constructor(holdMs: number) {
    this.#holdMs = holdMs;
  }

  /** Grab the surface's current frame and hold it. Resolves as `draw` does. */
  async capture(target: Target, element: OverlayRect): Promise<void> {
    const seq = this.#next();
    this.#target = target;
    await this.draw(target, element, () => seq === this.#seq);
  }

  /**
   * The held still's element now measures `element`: draw the surface again at that size.
   * Redraws run one at a time; a size that arrives while one is in flight replaces any still
   * waiting, and the one in flight lands only if nothing is waiting behind it. Until an
   * answer lands the old still keeps its size, and one that fails leaves it. Never rejects.
   */
  relayout(element: OverlayRect): Promise<void> {
    this.#nextSize = element;
    this.#redraws ??= this.#runRedraws();
    return this.#redraws;
  }

  /** The surface has been re-asserted: drop the still once the surface should cover it. */
  release(): void {
    const seq = this.#next();
    this.#releaseTimer = setTimeout(() => {
      if (seq === this.#seq) {
        this.frame = null;
      }
    }, this.#holdMs);
  }

  /** Drop the still now; nothing is coming back to cover the region. */
  clear(): void {
    this.#next();
    this.frame = null;
  }

  async #runRedraws(): Promise<void> {
    // Lets relayout() store this run before the loop can end it.
    await null;
    while (this.#nextSize) {
      const element = this.#nextSize;
      this.#nextSize = null;
      const held = this.frame;
      const target = this.#target;
      if (!held || target === undefined || sameSurfaceSize(held.element, element)) {
        continue;
      }
      const seq = this.#seq;
      await this.draw(target, element, () => seq === this.#seq && this.#nextSize === null);
    }
    this.#redraws = null;
  }

  // Every operation that decides what the region shows starts here: it voids whatever came
  // before, the release hold and any redraw included.
  #next(): number {
    clearTimeout(this.#releaseTimer);
    this.#target = undefined;
    this.#nextSize = null;
    return ++this.#seq;
  }
}
