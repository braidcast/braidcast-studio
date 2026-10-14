import { describe, expect, test } from "bun:test";
import { flushSync } from "svelte";
import { LiveSettings } from "$lib/settings/liveSettings.svelte";

interface S {
  a: number;
  b: string;
}

// A host the test answers by hand: every read and write waits until the test settles it.
class FakeHost {
  reads: { resolve: (s: S) => void; reject: (e: Error) => void }[] = [];
  writes: { patch: Partial<S>; resolve: (s: S) => void; reject: (e: Error) => void }[] = [];
  push: ((s: S) => void) | null = null;

  live(): LiveSettings<S> {
    return new LiveSettings<S>({
      read: () => new Promise<S>((resolve, reject) => this.reads.push({ resolve, reject })),
      write: (patch) => new Promise<S>((resolve, reject) => this.writes.push({ patch, resolve, reject })),
      listen: (onPush) => {
        this.push = onPush;
        return () => (this.push = null);
      },
      settingsOf: (s) => s,
      errorText: (e, fallback) => (e as Error).message || fallback,
    });
  }
}

const settle = async () => {
  for (let i = 0; i < 5; i++) {
    await Promise.resolve();
  }
  flushSync();
};

async function started(): Promise<{ host: FakeHost; live: LiveSettings<S>; stop: () => void }> {
  const host = new FakeHost();
  const live = host.live();
  const stop = live.start();
  host.reads[0].resolve({ a: 1, b: "x" });
  await settle();
  return { host, live, stop };
}

describe("LiveSettings", () => {
  test("shows nothing until the host answers, then its settings", async () => {
    const host = new FakeHost();
    const live = host.live();
    live.start();
    expect(live.settings).toBeNull();
    expect(live.readError).toBeNull();
    host.reads[0].resolve({ a: 1, b: "x" });
    await settle();
    expect(live.settings).toEqual({ a: 1, b: "x" });
  });

  // A read that fails must not leave defaults on screen as if they were the user's.
  test("a failed read is an error, never defaults; retry reads again", async () => {
    const host = new FakeHost();
    const live = host.live();
    live.start();
    host.reads[0].reject(new Error("host down"));
    await settle();
    expect(live.settings).toBeNull();
    expect(live.readError).toBe("host down");
    await live.apply({ a: 5 }); // nothing to change yet
    expect(host.writes.length).toBe(0);
    live.retry();
    expect(live.readError).toBeNull();
    host.reads[1].resolve({ a: 2, b: "y" });
    await settle();
    expect(live.settings).toEqual({ a: 2, b: "y" });
  });

  test("a push after a failed read clears the error", async () => {
    const host = new FakeHost();
    const live = host.live();
    live.start();
    host.reads[0].reject(new Error("host down"));
    await settle();
    host.push?.({ a: 3, b: "z" });
    expect(live.readError).toBeNull();
    expect(live.settings).toEqual({ a: 3, b: "z" });
  });

  test("a change shows at once and settles on the host's answer", async () => {
    const { host, live } = await started();
    const done = live.apply({ a: 2 });
    expect(live.settings).toEqual({ a: 2, b: "x" });
    expect(host.writes[0].patch).toEqual({ a: 2 });
    host.writes[0].resolve({ a: 2, b: "x2" });
    await done;
    expect(live.settings).toEqual({ a: 2, b: "x2" });
    expect(live.refused).toBeNull();
  });

  test("a refused change rolls back to what the host confirmed, and says why, for its key", async () => {
    const { host, live } = await started();
    const done = live.apply({ a: 9 });
    host.writes[0].reject(new Error("out of range"));
    await done;
    expect(live.settings).toEqual({ a: 1, b: "x" });
    expect(live.refused).toEqual({ keys: ["a"], message: "out of range" });
    expect(live.errorFor("a")).toBe("out of range");
    expect(live.errorFor("b")).toBe("");
    // The next change clears it.
    void live.apply({ b: "q" });
    expect(live.errorFor("a")).toBe("");
  });

  // Two changes in flight, the first refused: the rollback is to the host's confirmed
  // state, and only if nothing newer has claimed it -- the second change's answer wins.
  test("with two changes in flight, a refusal of the first does not undo the second", async () => {
    const { host, live } = await started();
    const first = live.apply({ a: 7 });
    const second = live.apply({ b: "new" });
    expect(live.settings).toEqual({ a: 7, b: "new" });
    host.writes[0].reject(new Error("no"));
    await first;
    expect(live.settings).toEqual({ a: 7, b: "new" }); // stale: the second owns the state
    host.writes[1].resolve({ a: 1, b: "new" });
    await second;
    expect(live.settings).toEqual({ a: 1, b: "new" });
  });

  test("with the second refused, it rolls back to the first's confirmed answer, not the control's value", async () => {
    const { host, live } = await started();
    const first = live.apply({ a: 7 });
    host.writes[0].resolve({ a: 7, b: "x" });
    await first;
    const second = live.apply({ a: 99 });
    host.writes[1].reject(new Error("too big"));
    await second;
    expect(live.settings).toEqual({ a: 7, b: "x" });
  });

  test("the host's push outranks a reply still in flight", async () => {
    const { host, live } = await started();
    const done = live.apply({ a: 4 });
    host.push?.({ a: 5, b: "pushed" });
    host.writes[0].resolve({ a: 4, b: "x" });
    await done;
    expect(live.settings).toEqual({ a: 5, b: "pushed" });
  });

  test("show() moves a value without writing it", async () => {
    const { host, live } = await started();
    live.show({ a: 3 });
    expect(live.settings).toEqual({ a: 3, b: "x" });
    expect(host.writes.length).toBe(0);
  });

  test("after stop, a late read and pushes are ignored", async () => {
    const host = new FakeHost();
    const live = host.live();
    const stop = live.start();
    stop();
    expect(host.push).toBeNull();
    host.reads[0].resolve({ a: 1, b: "x" });
    await settle();
    expect(live.settings).toBeNull();
  });
});
