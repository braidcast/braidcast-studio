import { describe, expect, test } from "bun:test";
import { flushSync } from "svelte";
import type { VoiceDraft } from "$lib/voice/voiceStatus";
import { DraftCountdown, type CountdownClock } from "$lib/voice/draftCountdown.svelte";
import { cell, root } from "./harness.svelte";

// A clock the test moves by hand; `every` keeps one timer at a time, as the countdown does.
class FakeClock implements CountdownClock {
  t = 1_000_000;
  timers = new Set<() => void>();
  now(): number {
    return this.t;
  }
  every(fn: () => void): () => void {
    this.timers.add(fn);
    return () => this.timers.delete(fn);
  }
  advance(ms: number): void {
    this.t += ms;
    for (const fn of [...this.timers]) {
      fn();
    }
    flushSync();
  }
}

function draft(overrides: Partial<VoiceDraft> = {}): VoiceDraft {
  return { id: 1, text: "gg", to: null, summary: "", phase: "countdown", remainingMs: 3000, limitSec: 3, ...overrides };
}

function mount(clock: FakeClock) {
  const source = cell<VoiceDraft | null>(null);
  let countdown: DraftCountdown | null = null;
  const stop = root(() => {
    countdown = new DraftCountdown(() => source.value, clock);
  });
  flushSync();
  return {
    source,
    get seconds() {
      return countdown!.seconds;
    },
    stop,
  };
}

describe("DraftCountdown", () => {
  test("counts down from what the host said was left", () => {
    const clock = new FakeClock();
    const c = mount(clock);
    c.source.value = draft({ remainingMs: 2500 });
    flushSync();
    expect(c.seconds).toBe(3);
    clock.advance(600);
    expect(c.seconds).toBe(2);
    clock.advance(1500);
    expect(c.seconds).toBe(1);
    clock.advance(500);
    expect(c.seconds).toBe(0);
    expect(clock.timers.size).toBe(0); // it stops itself at zero
    c.stop();
  });

  test("restarts from each emit's newer figure, never from a full window", () => {
    const clock = new FakeClock();
    const c = mount(clock);
    c.source.value = draft({ remainingMs: 3000 });
    flushSync();
    clock.advance(1000);
    expect(c.seconds).toBe(2);
    // The host emits again (a state change) 1.2 s in: 1.8 s left by ITS clock. The page
    // follows the host, not its own count, and keeps one timer.
    c.source.value = draft({ remainingMs: 1800 });
    flushSync();
    expect(c.seconds).toBe(2);
    expect(clock.timers.size).toBe(1);
    clock.advance(900);
    expect(c.seconds).toBe(1);
    // A draft that arrives late shows only what is left of it.
    c.source.value = draft({ id: 2, remainingMs: 400 });
    flushSync();
    expect(c.seconds).toBe(1);
    c.stop();
  });

  test("shows nothing, and runs no timer, while due or with no draft", () => {
    const clock = new FakeClock();
    const c = mount(clock);
    c.source.value = draft({ remainingMs: 3000 });
    flushSync();
    c.source.value = draft({ phase: "due", remainingMs: 0 });
    flushSync();
    expect(c.seconds).toBe(0);
    expect(clock.timers.size).toBe(0);
    c.source.value = null;
    flushSync();
    expect(c.seconds).toBe(0);
    c.stop();
  });

  test("a held draft counts down its window as well", () => {
    const clock = new FakeClock();
    const c = mount(clock);
    c.source.value = draft({ phase: "waiting", remainingMs: 8000, limitSec: 8 });
    flushSync();
    expect(c.seconds).toBe(8);
    c.stop();
    expect(clock.timers.size).toBe(0); // unmounting stops the timer
  });
});
