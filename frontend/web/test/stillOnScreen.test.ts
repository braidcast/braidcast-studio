import { describe, expect, test } from "bun:test";
import { PAINT_FRAMES, paintStill, stillOnScreen, type StillPaint } from "$lib/docking/freezeFrame";
import { nextPaintId, watchPresented, type ElementTimingSource } from "$lib/utils/presented";

// Settles every queued microtask and one macrotask turn, so a promise that could resolve
// from what has already happened has.
const settle = () => new Promise((r) => setTimeout(r, 0));

function fakeSource(): { source: ElementTimingSource; emit: (id: string) => void; listening: () => number } {
  const listeners = new Set<(id: string) => void>();
  return {
    source: (onEntry) => {
      listeners.add(onEntry);
      return () => listeners.delete(onEntry);
    },
    emit: (id) => [...listeners].forEach((l) => l(id)),
    listening: () => listeners.size,
  };
}

describe("stillOnScreen", () => {
  // The flicker: decode and frame boundaries all done, but the frame showing the still not
  // yet presented. Hiding the surface then exposes whatever the web view last presented.
  test("waits for the presentation, not for frames, when it can be observed", async () => {
    let resolvePresented!: () => void;
    const steps = {
      presented: new Promise<void>((r) => (resolvePresented = r)),
      decode: () => Promise.resolve(),
      nextFrame: () => Promise.resolve(),
      current: () => true,
    };
    let done = false;
    void stillOnScreen(steps, 4).then(() => (done = true));
    await settle();
    expect(done).toBe(false);
    resolvePresented();
    await settle();
    expect(done).toBe(true);
  });

  test("falls back to decode and the given number of frames", async () => {
    const order: string[] = [];
    await stillOnScreen(
      {
        presented: null,
        decode: async () => void order.push("decode"),
        nextFrame: async () => void order.push("frame"),
        current: () => true,
      },
      4,
    );
    expect(order).toEqual(["decode", "frame", "frame", "frame", "frame"]);
  });

  test("stops counting frames once overtaken", async () => {
    let frames = 0;
    await stillOnScreen(
      { presented: null, decode: async () => {}, nextFrame: async () => void frames++, current: () => frames < 1 },
      4,
    );
    expect(frames).toBe(1);
  });
});

describe("watchPresented", () => {
  test("resolves on its own identifier only, then stops listening", async () => {
    const fake = fakeSource();
    const watch = watchPresented("freeze-2", fake.source);
    let done = false;
    void watch.presented.then(() => (done = true));
    fake.emit("freeze-1");
    await settle();
    expect(done).toBe(false);
    fake.emit("freeze-2");
    await settle();
    expect(done).toBe(true);
    expect(fake.listening()).toBe(0);
  });

  test("stop() detaches a watch that never saw its entry", () => {
    const fake = fakeSource();
    watchPresented("freeze-3", fake.source).stop();
    expect(fake.listening()).toBe(0);
  });
});

describe("nextPaintId", () => {
  // Each dock owns a PreviewFreeze and they all capture on the same gate flip, so a
  // per-instance id would repeat across docks and one dock's entry would answer another's.
  test("never repeats within the document", () => {
    const ids = new Set(Array.from({ length: 50 }, () => nextPaintId()));
    expect(ids.size).toBe(50);
  });
});

test("PAINT_FRAMES stays at the measured safe count", () => {
  // Two frames lost the race in 35 of 40 trials; four in none of 20 (see its comment).
  expect(PAINT_FRAMES).toBeGreaterThanOrEqual(4);
});

describe("paintStill", () => {
  function plan(over: Partial<StillPaint> & { source: ElementTimingSource | null }): {
    p: StillPaint;
    inserted: string[];
    frames: () => number;
  } {
    const inserted: string[] = [];
    let frames = 0;
    const p: StillPaint = {
      heldUri: undefined,
      nextUri: "data:new",
      insert: async (id) => {
        inserted.push(id);
        return { decode: () => Promise.resolve() };
      },
      bound: (step) => step.then(() => {}),
      nextFrame: async () => void frames++,
      current: () => true,
      ...over,
    };
    return { p, inserted, frames: () => frames };
  }

  test("watches before inserting, and hides only on its own entry", async () => {
    const fake = fakeSource();
    let listeningAtInsert = -1;
    const { p, inserted, frames } = plan({ source: fake.source });
    const insert = p.insert;
    p.insert = (id) => {
      listeningAtInsert = fake.listening();
      return insert(id);
    };
    let outcome: string | undefined;
    void paintStill(p).then((o) => (outcome = o));
    await settle();
    expect(listeningAtInsert).toBe(1);
    // Decode and frames are instant here; only the presentation entry may release it.
    expect(outcome).toBeUndefined();
    expect(frames()).toBe(0);
    fake.emit(inserted[0]);
    await settle();
    expect(outcome).toBe("presented");
    expect(fake.listening()).toBe(0);
  });

  test("another dock's still being presented does not release this one", async () => {
    const fake = fakeSource();
    const a = plan({ source: fake.source });
    const b = plan({ source: fake.source });
    let doneA = false;
    let doneB = false;
    void paintStill(a.p).then(() => (doneA = true));
    void paintStill(b.p).then(() => (doneB = true));
    await settle();
    expect(a.inserted[0]).not.toBe(b.inserted[0]);
    fake.emit(a.inserted[0]);
    await settle();
    expect([doneA, doneB]).toEqual([true, false]);
    fake.emit(b.inserted[0]);
    await settle();
    expect(doneB).toBe(true);
  });

  test("the picture already on show gets no watch and falls back to frames", async () => {
    const fake = fakeSource();
    // A bound that runs out, so a wrongly installed watch ends as "timeout" rather than hanging.
    const bound = (step: Promise<unknown>) =>
      Promise.race([step.then(() => {}), new Promise<void>((r) => setTimeout(r, 20))]);
    const { p, frames } = plan({ source: fake.source, heldUri: "data:same", nextUri: "data:same", bound });
    let listening = -1;
    const insert = p.insert;
    p.insert = (id) => {
      listening = fake.listening();
      return insert(id);
    };
    expect(await paintStill(p)).toBe("frames");
    expect(listening).toBe(0);
    expect(frames()).toBe(PAINT_FRAMES);
  });

  test("without element timing it falls back to frames", async () => {
    const { p, frames } = plan({ source: null });
    expect(await paintStill(p)).toBe("frames");
    expect(frames()).toBe(PAINT_FRAMES);
  });

  test("a presentation that never comes is cut off by the bound and the watch detached", async () => {
    const fake = fakeSource();
    const { p } = plan({ source: fake.source, bound: () => Promise.resolve() });
    expect(await paintStill(p)).toBe("timeout");
    expect(fake.listening()).toBe(0);
  });

  test("overtaken during insert: no wait, watch detached", async () => {
    const fake = fakeSource();
    const { p, frames } = plan({ source: fake.source, current: () => false });
    expect(await paintStill(p)).toBe("overtaken");
    expect(frames()).toBe(0);
    expect(fake.listening()).toBe(0);
  });
});
