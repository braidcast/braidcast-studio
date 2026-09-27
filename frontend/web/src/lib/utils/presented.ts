// Knowing that something the page drew has reached the screen, not just been scheduled for
// it: what a native surface waits on before it leaves a region to the web view.

/** Hands each element-timing entry's identifier to `onEntry`; the returned function stops it. */
export type ElementTimingSource = (onEntry: (identifier: string) => void) => () => void;

// Element Timing queues an image's entry from the presentation feedback of the frame that
// first painted it, so the entry arriving is the one sign a page gets that a picture has
// reached the screen rather than been scheduled for it. Null where it is not supported.
export function elementTimingSource(): ElementTimingSource | null {
  if (typeof PerformanceObserver === "undefined" || !PerformanceObserver.supportedEntryTypes?.includes("element")) {
    return null;
  }
  return (onEntry) => {
    const observer = new PerformanceObserver((list) => {
      for (const entry of list.getEntries()) {
        onEntry((entry as PerformanceEntry & { identifier?: string }).identifier ?? "");
      }
    });
    observer.observe({ type: "element" });
    return () => observer.disconnect();
  };
}

// Every observer sees every element entry in the document -- each dock's still, each floating
// layer's sentinel -- so an id drawn from a per-owner counter would let one's presentation
// answer for another's. One counter for the document.
let paintSerial = 0;

/** An `elementtiming` identifier nothing else in this document carries. */
export function nextPaintId(): string {
  return `paint-${++paintSerial}`;
}

/** Watch for `identifier` to be presented. Start it before the element is inserted. */
export function watchPresented(identifier: string, source: ElementTimingSource): { presented: Promise<void>; stop: () => void } {
  let stop = () => {};
  const presented = new Promise<void>((resolve) => {
    stop = source((id) => {
      if (id === identifier) {
        stop();
        resolve();
      }
    });
  });
  return { presented, stop: () => stop() };
}

// Bound on one step of waiting for the screen (a presentation, a decode, a frame). Whatever
// waits on these goes ahead once they finish, and nothing guarantees one settles -- rAF does
// not run while the web view is hidden, and a presentation entry can simply not come -- so
// an unbounded step would leave the native surface drawn over the layer it is waiting to
// show. A step that runs out just moves on: a layer that lands late beats one never shown.
const PRESENT_STEP_MS = 150;

export function bounded(step: Promise<unknown>): Promise<void> {
  return new Promise((resolve) => {
    const timer = setTimeout(resolve, PRESENT_STEP_MS);
    const done = () => {
      clearTimeout(timer);
      resolve();
    };
    step.then(done, done);
  });
}

/** The next animation frame, bounded like any other step. */
export function nextFrame(): Promise<void> {
  return bounded(new Promise((resolve) => requestAnimationFrame(resolve)));
}
