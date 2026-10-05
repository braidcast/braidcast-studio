import { describe, expect, test } from "bun:test";

// The default emote wall, loaded as source against stand-ins for the globals it touches.
// Motion is inert here (no Web Animations); what is tested is which emotes go up, how many,
// and which leave.

const SOURCE = await Bun.file(new URL("../public/overlay/default-emotewall/template.js", import.meta.url)).text();

class El {
  className = "";
  alt = "";
  src = "";
  clientWidth = 1920;
  clientHeight = 1080;
  offsetWidth = 72;
  children: El[] = [];
  parent: El | null = null;
  appendChild(c: El) {
    c.parent = this;
    this.children.push(c);
    return c;
  }
  remove() {
    if (this.parent) this.parent.children = this.parent.children.filter((c) => c !== this);
  }
}

function wall(fields: Record<string, unknown> = {}) {
  const ids: Record<string, El> = {};
  const document = {
    getElementById: (id: string) => (ids[id] ??= new El()),
    documentElement: { style: { setProperty() {} } },
    createElement: () => new El(),
  };
  const handlers: Record<string, (...x: unknown[]) => void> = {};
  const on = (name: string) => (fn: (...x: unknown[]) => void) => (handlers[name] = fn);
  const overlay = {
    preview: false,
    onLoad: on("load"),
    onChat: on("chat"),
    onChatModeration: on("moderation"),
    chatIdentity: (m: { id: string }) => ({ id: m.id }),
  };
  const timers: (() => void)[] = [];
  new Function("document", "OBSOverlay", "window", "setTimeout", "clearTimeout", SOURCE)(
    document,
    overlay,
    {},
    (fn: () => void) => timers.push(fn),
    () => {},
  );
  handlers.load({ fields });
  return {
    shown: () => ids.wall.children.map((c) => c.src),
    chat: (m: unknown) => handlers.chat(m),
    moderate: (removedId: string) => handlers.moderation({}, (id: { id: string }) => id.id === removedId),
    expireAll: () => timers.splice(0).forEach((fn) => fn()),
  };
}

const emote = (code: string, url = `https://cdn.example/${code}.png`) => ({ type: "emote", code, url });
const line = (id: string, platform: string, ...fragments: unknown[]) => ({ id, platform, fragments });

describe("emote wall", () => {
  test("throws a message's emotes, capped per message and on screen", () => {
    const w = wall({ perMessage: 2, maxOnScreen: 5 });
    w.chat(line("1", "twitch", { type: "text", text: "hi " }, emote("A"), emote("B"), emote("C")));
    expect(w.shown()).toEqual(["https://cdn.example/A.png", "https://cdn.example/B.png"]);
    w.chat(line("2", "twitch", emote("D"), emote("E")));
    w.chat(line("3", "twitch", emote("F"), emote("G")));
    // Five at most: the oldest, A, left early.
    const url = (c: string) => `https://cdn.example/${c}.png`;
    expect(w.shown()).toEqual(["B", "D", "E", "F", "G"].map(url));
  });

  test("an emote leaves after its time on screen, and a removed message's emotes leave at once", () => {
    const w = wall();
    w.chat(line("1", "twitch", emote("A")));
    w.chat(line("2", "twitch", emote("B")));
    w.moderate("1");
    expect(w.shown()).toEqual(["https://cdn.example/B.png"]);
    w.expireAll();
    expect(w.shown()).toEqual([]);
  });

  test("only https emotes from the platforms switched on", () => {
    const w = wall({ showKick: false });
    w.chat(line("1", "kick", emote("K")));
    w.chat(line("2", "youtube", emote("Y", "http://insecure.example/y.png"), emote("Z")));
    expect(w.shown()).toEqual(["https://cdn.example/Z.png"]);
  });
});
