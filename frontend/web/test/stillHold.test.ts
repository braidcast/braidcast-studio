import { describe, expect, test } from "bun:test";
import type { FrozenFrame } from "$lib/docking/freezeFrame";
import { StillHold } from "$lib/docking/stillHold";
import type { OverlayRect } from "$lib/utils/overlayRect";

const settle = () => new Promise((r) => setTimeout(r, 0));
const HOLD_MS = 5;

function rect(w: number, h: number): OverlayRect {
  return { x: 0, y: 0, w, h, dpr: 1 };
}

function stillAt(element: OverlayRect): FrozenFrame {
  return { dataUri: `still-${element.w}x${element.h}`, element } as unknown as FrozenFrame;
}

// A hold whose draws the test answers by hand; an answer lands only while `current()` holds,
// as PreviewFreeze's does.
class TestHold extends StillHold<string> {
  frame: FrozenFrame | null = null;
  readonly draws: { element: OverlayRect; land: () => void }[] = [];

  constructor() {
    super(HOLD_MS);
  }

  protected draw(_target: string, element: OverlayRect, current: () => boolean): Promise<void> {
    return new Promise((resolve) => {
      this.draws.push({
        element,
        land: () => {
          if (current()) {
            this.frame = stillAt(element);
          }
          resolve();
        },
      });
    });
  }
}

async function held(): Promise<TestHold> {
  const hold = new TestHold();
  const captured = hold.capture("canvas", rect(800, 450));
  hold.draws[0].land();
  await captured;
  return hold;
}

describe("StillHold", () => {
  test("a resize burst draws one size at a time, and the latest size wins", async () => {
    const hold = await held();
    void hold.relayout(rect(900, 500));
    await settle();
    void hold.relayout(rect(1000, 560));
    void hold.relayout(rect(1100, 620));
    await settle();
    // One capture in flight, however many sizes came in behind it.
    expect(hold.draws.length).toBe(2);
    hold.draws[1].land();
    await settle();
    // Overtaken while in flight: the in-between size never reaches the screen.
    expect(hold.frame!.element.w).toBe(800);
    expect(hold.draws.length).toBe(3);
    expect(hold.draws[2].element.w).toBe(1100);
    hold.draws[2].land();
    await settle();
    expect(hold.frame!.element.w).toBe(1100);
  });

  test("a resize back to the held size voids the answer in flight for the size between", async () => {
    const hold = await held();
    void hold.relayout(rect(900, 500));
    await settle();
    void hold.relayout(rect(800, 450));
    hold.draws[1].land();
    await settle();
    expect(hold.frame!.element.w).toBe(800);
    expect(hold.draws.length).toBe(2);
  });

  test("a redraw answered after the release hold has let go puts no still back", async () => {
    const hold = await held();
    hold.release();
    // The surface is live again: a resize during the hold has nothing to stand in for.
    void hold.relayout(rect(900, 500));
    await new Promise((r) => setTimeout(r, HOLD_MS * 4));
    expect(hold.frame).toBeNull();
    expect(hold.draws.length).toBe(1);
  });

  test("a redraw in flight as the surface is released lands nowhere", async () => {
    const hold = await held();
    void hold.relayout(rect(900, 500));
    await settle();
    hold.release();
    await new Promise((r) => setTimeout(r, HOLD_MS * 4));
    hold.draws[1].land();
    await settle();
    expect(hold.frame).toBeNull();
  });

  test("a new capture during the hold keeps its still past the old hold", async () => {
    const hold = await held();
    hold.release();
    const recaptured = hold.capture("canvas", rect(800, 450));
    hold.draws[1].land();
    await recaptured;
    await new Promise((r) => setTimeout(r, HOLD_MS * 4));
    expect(hold.frame).not.toBeNull();
  });
});
