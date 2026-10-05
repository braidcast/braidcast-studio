import { describe, expect, test } from "bun:test";

// The default stream boss, loaded as source against stand-ins for the globals it touches.

const SOURCE = await Bun.file(new URL("../public/overlay/default-streamboss/template.js", import.meta.url)).text();

class El {
  textContent = "";
  offsetWidth = 0;
  style = { width: "" };
  classes = new Set<string>();
  classList = {
    add: (c: string) => this.classes.add(c),
    remove: (...cs: string[]) => cs.forEach((c) => this.classes.delete(c)),
  };
}

function boss(fields: Record<string, unknown> = {}, preview = false) {
  const ids: Record<string, El> = {};
  const document = { getElementById: (id: string) => (ids[id] ??= new El()), documentElement: { style: { setProperty() {} } } };
  const handlers: Record<string, (x: unknown) => void> = {};
  const on = (name: string) => (fn: (x: unknown) => void) => (handlers[name] = fn);
  const overlay = {
    preview,
    onLoad: on("load"),
    onEvent: on("event"),
    onBackfill: on("backfill"),
    formatCount: (n: number) => String(n),
  };
  new Function("document", "OBSOverlay", SOURCE)(document, overlay);
  handlers.load({ fields: { startHp: 100, hpGrowth: 50, ...fields } });
  return {
    read: () => `${ids.name.textContent} ${ids.hp.textContent}`,
    hit: () => ids.hit.textContent,
    card: () => ids.boss,
    fire: (e: unknown) => handlers.event(e),
    backfill: (l: unknown[]) => handlers.backfill(l),
  };
}

const ev = (id: string, type: string, actorName: string, extra: Record<string, unknown> = {}) => ({
  id,
  type,
  platform: "twitch",
  ts: 0,
  actorName,
  ...extra,
});

describe("stream boss", () => {
  test("events deal their damage, and the final hit's dealer becomes a tougher boss", () => {
    const b = boss();
    expect(b.read()).toBe("The Streamer 100 / 100 HP");
    b.fire(ev("a", "follow", "Ann")); // 10
    b.fire(ev("b", "cheer", "Bo", { amount: 30 })); // 30
    expect(b.read()).toBe("The Streamer 60 / 100 HP");
    expect(b.hit()).toBe("Bo hit for 30");
    b.fire(ev("c", "sub", "Cy")); // 100: overkill
    expect(b.read()).toBe("Cy 150 / 150 HP");
    expect(b.hit()).toBe("Cy defeated The Streamer!");
    expect(b.card().classes.has("new-boss")).toBe(true);
    b.fire(ev("d", "subgift", "Di", { count: 2 })); // 2 subs: 200
    expect(b.read()).toBe("Di 200 / 200 HP");
  });

  test("a reloaded source rebuilds the same boss from the backfill, counting each event once", () => {
    const list = [ev("a", "sub", "Ann"), ev("b", "follow", "Bo")];
    const b = boss();
    b.backfill(list);
    b.fire(ev("b", "follow", "Bo"));
    expect(b.read()).toBe("Ann 140 / 150 HP");
    expect(b.hit()).toBe("");
  });

  test("replays deal nothing; test hits land only in the editor preview", () => {
    const b = boss();
    b.fire(ev("a", "follow", "Ann", { replay: true }));
    b.fire(ev("t", "follow", "Tester", { test: true }));
    expect(b.read()).toBe("The Streamer 100 / 100 HP");
    const p = boss({}, true);
    p.fire(ev("t", "follow", "Tester", { test: true }));
    expect(p.read()).toBe("The Streamer 90 / 100 HP");
  });

  test("a damage set to zero ignores that event", () => {
    const b = boss({ dmgFollow: 0, bossName: "Me" });
    b.fire(ev("a", "follow", "Ann"));
    expect(b.read()).toBe("Me 100 / 100 HP");
  });
});
