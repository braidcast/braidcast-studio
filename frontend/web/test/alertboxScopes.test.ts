import { describe, expect, test } from "bun:test";
import { ALERTBOX_DEFAULTS, ALERTBOX_SOURCE, alertboxOverlay, type StubScopes } from "./alertboxStub";

// The default alert box against scoped settings: the template asks OBSOverlay.resolveAlert
// for each alert, and the stub answers with the real resolver, so these cover the template's
// half of the contract -- which alert's resolution a burst and a card take.

interface Burst {
  look: Record<string, unknown>;
  events: { type: string; amount?: number }[];
}

function part() {
  return { textContent: "" };
}

function card() {
  const parts: Record<string, { textContent: string }> = {
    ".alert-name": part(),
    ".alert-msg": part(),
    ".alert-media": part(),
  };
  return {
    dataset: {} as Record<string, string>,
    querySelector: (sel: string) => parts[sel],
    text: () => `${parts[".alert-name"].textContent} | ${parts[".alert-msg"].textContent}`,
  };
}

function alertbox(scopes: StubScopes) {
  let handler: (e: unknown) => void = () => {};
  const el = () => ({
    dataset: {},
    style: { setProperty() {} },
    classList: { add() {}, remove() {}, toggle() {} },
    querySelector: () => part(),
    querySelectorAll: () => [],
    insertBefore() {},
    content: { firstElementChild: { cloneNode: () => el() } },
  });
  const document = { getElementById: () => el(), body: el() };
  const overlay = alertboxOverlay(ALERTBOX_DEFAULTS, scopes, {
    onEvent: (fn: typeof handler) => (handler = fn),
    formatAmountText: (e: { amount?: number }) => `${e.amount} bits`,
  });
  const run = new Function(
    "document",
    "OBSOverlay",
    "performance",
    "setTimeout",
    "clearTimeout",
    ALERTBOX_SOURCE + "\n;return { queue: () => queue, current: () => current, fillCard };",
  );
  const box = run(document, overlay, { now: () => 0 }, () => 0, () => {}) as {
    queue: () => Burst[];
    current: () => Burst | null;
    fillCard: (el: unknown, e: unknown, look: unknown) => void;
  };
  return { box, fire: (e: Record<string, unknown>) => handler({ id: "x", actorName: "viewer", ...e }) };
}

const SCOPES: StubScopes = {
  overrides: { cheer: { sound: "library:coin-01", message: "{name} threw {amountText}" } },
  variations: [
    {
      id: "v_big",
      event: "cheer",
      when: { field: "amount", op: ">=", value: 1000 },
      settings: { sound: "library:jingle-01", message: "{name} made it rain {amountText}" },
    },
  ],
};

describe("alert box scopes", () => {
  test("a burst takes its look from the scope its first alert resolves to", () => {
    const a = alertbox(SCOPES);
    a.fire({ type: "cheer", amount: 1500 });
    a.fire({ type: "cheer", amount: 10 });
    a.fire({ type: "follow" });
    expect(a.box.current()?.look.sound).toBe("library:jingle-01");
    const [small, follow] = a.box.queue();
    expect(small.look.sound).toBe("library:coin-01");
    expect(follow.look.sound).toBe(ALERTBOX_DEFAULTS.sound);
  });

  test("each card says its own alert's resolved message, without the name it opens with", () => {
    const a = alertbox(SCOPES);
    const look = ALERTBOX_DEFAULTS;
    const big = card();
    a.box.fillCard(big, { type: "cheer", amount: 1500, actorName: "viewer" }, look);
    expect(big.text()).toBe("viewer | made it rain 1500 bits");
    const small = card();
    a.box.fillCard(small, { type: "cheer", amount: 10, actorName: "viewer" }, look);
    expect(small.text()).toBe("viewer | threw 10 bits");
    const follow = card();
    a.box.fillCard(follow, { type: "follow", actorName: "" }, look);
    expect(follow.text()).toBe("Someone | just followed!");
  });

  test("no media, or the text-only layout, leaves a text-only card", () => {
    const a = alertbox({});
    const c = card();
    a.box.fillCard(c, { type: "follow", actorName: "v" }, { ...ALERTBOX_DEFAULTS, media: "/w/x/assets/a.png", layout: "none" });
    expect(c.dataset.layout).toBe("none");
    const d = card();
    a.box.fillCard(d, { type: "follow", actorName: "v" }, ALERTBOX_DEFAULTS);
    expect(d.dataset.layout).toBe("none");
  });
});
