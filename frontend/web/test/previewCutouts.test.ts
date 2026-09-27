import { describe, expect, test } from "bun:test";
import {
  CutoutSet,
  SEND_ATTEMPTS,
  holdUntilRestored,
  intersectCut,
  mayShowThrough,
  sameCut,
  type CutRect,
  type CutoutState,
} from "$lib/docking/previewCutouts";

const settle = () => new Promise((r) => setTimeout(r, 0));

// A CutoutSet whose presentations, host replies and retry delays the test releases by hand.
// A reply is the call landing, by default with every rect over a surface, or failing.
type Reply = (ok?: boolean, hits?: boolean[]) => void;

function harness() {
  const sent: CutoutState[] = [];
  const replies: Reply[] = [];
  const presentations: (() => void)[] = [];
  const retries: (() => void)[] = [];
  const set = new CutoutSet({
    send(state) {
      sent.push(state);
      return new Promise<boolean[] | null>((resolve) =>
        replies.push((ok = true, hits) => resolve(ok ? (hits ?? state.rects.map(() => true)) : null)),
      );
    },
    presented() {
      return new Promise<void>((resolve) => presentations.push(resolve));
    },
    retryDelay() {
      return new Promise<void>((resolve) => retries.push(resolve));
    },
  });
  const rects = () => sent.map((s) => s.rects);
  return { set, sent, rects, replies, presentations, retries };
}

const MENU: CutRect = { x: 100, y: 80, w: 180, h: 240 };
const FLYOUT: CutRect = { x: 278, y: 120, w: 160, h: 90 };

// One layer, opened and cut at MENU with every reply answered.
async function openMenu(h: ReturnType<typeof harness>) {
  const layer = h.set.add();
  void h.set.reshape(layer, MENU);
  h.presentations.at(-1)!();
  await settle();
  for (const reply of h.replies) {
    reply();
  }
  await settle();
  return layer;
}

describe("intersectCut", () => {
  test("is the overlap, or null when there is none", () => {
    expect(intersectCut(MENU, { x: 200, y: 0, w: 500, h: 100 })).toEqual({ x: 200, y: 80, w: 80, h: 20 });
    expect(intersectCut(MENU, { x: 280, y: 80, w: 10, h: 10 })).toBeNull();
  });

  test("sameCut compares by value", () => {
    expect(sameCut(MENU, { ...MENU })).toBe(true);
    expect(sameCut(MENU, null)).toBe(false);
    expect(sameCut(null, null)).toBe(true);
  });
});

describe("CutoutSet", () => {
  test("grabs the pointer as a layer opens, and cuts it only once it has been presented", async () => {
    const h = harness();
    const layer = h.set.add();
    // A press between opening and the first presentation must already go to the layer.
    expect(h.sent).toEqual([{ rects: [], grab: true }]);
    const shaped = h.set.reshape(layer, MENU);
    await settle();
    // Cutting now would show the page from before the menu.
    expect(h.sent.length).toBe(1);
    h.presentations[0]();
    await settle();
    expect(h.sent.at(-1)).toEqual({ rects: [MENU], grab: true });
    h.replies.forEach((reply) => reply());
    await shaped;
  });

  test("gives back a lost part at once and cuts a gained part only once presented", async () => {
    const h = harness();
    const layer = await openMenu(h);
    // Shorter: the strip it left is page content the next frame no longer covers.
    const shorter = { ...MENU, h: 100 };
    void h.set.reshape(layer, shorter);
    expect(h.rects().at(-1)).toEqual([shorter]);
    // Wider and taller than that: only the overlap is kept until the new shape is presented.
    const wider = { ...MENU, w: 300, h: 150 };
    const sends = h.sent.length;
    void h.set.reshape(layer, wider);
    expect(h.sent.length).toBe(sends);
    h.presentations.at(-1)!();
    await settle();
    expect(h.rects().at(-1)).toEqual([wider]);
  });

  test("a presentation for a shape since replaced lands nowhere", async () => {
    const h = harness();
    const layer = h.set.add();
    void h.set.reshape(layer, MENU);
    const moved = { ...MENU, h: 300 };
    void h.set.reshape(layer, moved);
    h.presentations[0]();
    await settle();
    expect(h.rects()).toEqual([[]]);
    h.presentations[1]();
    await settle();
    expect(h.rects()).toEqual([[], [moved]]);
  });

  test("release resolves only once the host has closed the hole, and ends the grab with the last layer", async () => {
    const h = harness();
    const layer = await openMenu(h);
    let closed = false;
    const release = h.set.release(layer).then(() => {
      closed = true;
    });
    expect(h.sent.at(-1)).toEqual({ rects: [], grab: false });
    await settle();
    // Whatever keeps the layer painted waits on this; resolving before the host replied would
    // let the page present the frame without the layer while the hole is still open.
    expect(closed).toBe(false);
    h.replies.at(-1)!();
    await release;
    expect(closed).toBe(true);
    // Idempotent: no second restore.
    const sends = h.sent.length;
    await h.set.release(layer);
    expect(h.sent.length).toBe(sends);
  });

  test("a layer released before it was presented never cuts", async () => {
    const h = harness();
    const layer = h.set.add();
    void h.set.reshape(layer, MENU);
    const release = h.set.release(layer);
    h.replies.forEach((reply) => reply());
    await release;
    h.presentations[0]();
    await settle();
    expect(h.rects().every((r) => r.length === 0)).toBe(true);
  });

  test("layers union, and one leaving keeps the other's hole and the grab", async () => {
    const h = harness();
    const root = await openMenu(h);
    const flyout = h.set.add();
    void h.set.reshape(flyout, FLYOUT);
    h.presentations.at(-1)!();
    await settle();
    expect(h.sent.at(-1)).toEqual({ rects: [MENU, FLYOUT], grab: true });
    void h.set.release(flyout);
    expect(h.sent.at(-1)).toEqual({ rects: [MENU], grab: true });
    void h.set.release(root);
    expect(h.sent.at(-1)).toEqual({ rects: [], grab: false });
  });

  test("resend repeats an unchanged state, for a new scale", async () => {
    const h = harness();
    const layer = await openMenu(h);
    const sends = h.sent.length;
    void h.set.reshape(layer, MENU);
    expect(h.sent.length).toBe(sends);
    void h.set.resend();
    expect(h.sent.length).toBe(sends + 1);
    expect(h.sent.at(-1)).toEqual(h.sent.at(-2)!);
  });

  test("a failed send is tried again, and a release waits for the restore that lands", async () => {
    const h = harness();
    const layer = await openMenu(h);
    let closed = false;
    const release = h.set.release(layer).then(() => {
      closed = true;
    });
    h.replies.at(-1)!(false);
    await settle();
    // The hole is still open: nothing may treat the layer as gone.
    expect(closed).toBe(false);
    const sends = h.sent.length;
    h.retries[0]();
    await settle();
    expect(h.sent.length).toBe(sends + 1);
    expect(h.sent.at(-1)).toEqual({ rects: [], grab: false });
    expect(closed).toBe(false);
    h.replies.at(-1)!();
    await release;
    expect(closed).toBe(true);
  });

  test("a retry sends the state as it is by then, not the one that failed", async () => {
    const h = harness();
    const root = await openMenu(h);
    const flyout = h.set.add();
    void h.set.reshape(flyout, FLYOUT);
    h.presentations.at(-1)!();
    await settle();
    h.replies.forEach((reply) => reply());
    void h.set.release(flyout);
    h.replies.at(-1)!(false);
    await settle();
    void h.set.release(root);
    // Released during the failed send: the next send carries both changes.
    expect(h.sent.at(-1)).toEqual({ rects: [], grab: false });
  });

  test("gives up after SEND_ATTEMPTS, so nothing waits on a host that keeps refusing", async () => {
    const h = harness();
    const layer = await openMenu(h);
    const release = h.set.release(layer);
    for (let attempt = 1; attempt <= SEND_ATTEMPTS; attempt++) {
      h.replies.at(-1)!(false);
      await settle();
      if (attempt < SEND_ATTEMPTS) {
        h.retries.at(-1)!();
        await settle();
      }
    }
    await release;
    expect(h.retries.length).toBe(SEND_ATTEMPTS - 1);
  });
});

describe("mayShowThrough", () => {
  test("a layer the host found clear of every surface has no hole to show through", async () => {
    const h = harness();
    const layer = h.set.add();
    void h.set.reshape(layer, MENU);
    // Not cut yet: nothing can show through.
    expect(mayShowThrough(layer)).toBe(false);
    h.presentations[0]();
    await settle();
    // Sent, not answered: whether it opened a hole is unknown, so it may have.
    expect(mayShowThrough(layer)).toBe(true);
    h.replies[0]();
    h.replies[1](true, [false]);
    await settle();
    expect(mayShowThrough(layer)).toBe(false);
  });

  test("the answer that decides is the one for the layer's latest shape", async () => {
    const h = harness();
    const layer = await openMenu(h);
    expect(mayShowThrough(layer)).toBe(true);
    const shorter = { ...MENU, h: 100 };
    void h.set.reshape(layer, shorter);
    const grown = { ...shorter, w: 300 };
    void h.set.reshape(layer, grown);
    h.presentations.at(-1)!();
    await settle();
    // Two sends out; the first answered "clear" says nothing about the grown rect behind it.
    h.replies.at(-2)!(true, [false]);
    await settle();
    expect(mayShowThrough(layer)).toBe(true);
    h.replies.at(-1)!(true, [false]);
    await settle();
    expect(mayShowThrough(layer)).toBe(false);
  });

  // A menu with a flyout open, neither over a surface, closing as one: both levels release in
  // the same teardown, and the first release resends the set with the other level still in it.
  for (const flyoutFirst of [true, false]) {
    test(`a two-level menu clear of every surface closes at once (${flyoutFirst ? "flyout" : "root"} released first)`, async () => {
      const h = harness();
      const root = h.set.add();
      void h.set.reshape(root, MENU);
      const flyout = h.set.add();
      void h.set.reshape(flyout, FLYOUT);
      for (const present of h.presentations) {
        present();
      }
      await settle();
      for (const reply of h.replies) {
        reply(true, [false, false]);
      }
      await settle();
      const [first, second] = flyoutFirst ? [flyout, root] : [root, flyout];
      expect(mayShowThrough(first)).toBe(false);
      void h.set.release(first);
      // The resend carrying the other level's unchanged rect must not cost it its answer.
      expect(mayShowThrough(second)).toBe(false);
      void h.set.release(second);
    });
  }

  test("a failed send leaves it unknown", async () => {
    const h = harness();
    const layer = h.set.add();
    void h.set.reshape(layer, MENU);
    h.presentations[0]();
    await settle();
    h.replies[0]();
    h.replies[1](false);
    await settle();
    expect(mayShowThrough(layer)).toBe(true);
  });
});

describe("holdUntilRestored", () => {
  test("a leaving flyout's likeness stays until the host has closed its hole", async () => {
    const h = harness();
    await openMenu(h);
    const flyout = h.set.add();
    void h.set.reshape(flyout, FLYOUT);
    h.presentations.at(-1)!();
    await settle();
    h.replies.forEach((reply) => reply());
    // Swapped by hover: the flyout has left the page, its likeness is up.
    let uncovered = false;
    const held = holdUntilRestored(h.set.release(flyout), () => (uncovered = true), 1000);
    await settle();
    expect(uncovered).toBe(false);
    h.replies.at(-1)!();
    await held;
    expect(uncovered).toBe(true);
  });

  test("a host that never answers cannot keep the likeness up", async () => {
    let uncovered = 0;
    await holdUntilRestored(new Promise(() => {}), () => uncovered++, 5);
    expect(uncovered).toBe(1);
  });
});
