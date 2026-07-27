// The clean native widget API injected into every served overlay document. Runs
// inside an OBS Browser Source (its own CEF process), NOT the app. Reads the
// host-injected window.__OVERLAY__ = {id, token, port, fields} and streams
// NormalizedEvents over SSE. Compiled to dist/overlay/runtime.js by bun build.

// ChatHub fans ONE payload object to both the overlay server and the bridge, so the
// wire shape here is the bridge's shape. Type-only, so it erases at build time and the
// runtime still bundles standalone (no bridge module pulled in).
import type { ChatMessage, NormalizedEvent, ViewerCounts } from "$lib/api/bridge";

interface OverlayBootstrap {
  id: string;
  token: string;
  port: number;
  fields: Record<string, unknown>;
}

/** A viewer-count cycle as a widget sees it: the host payload verbatim, plus the
 * per-platform sum every widget would otherwise derive for itself. */
export interface ViewerSnapshot extends ViewerCounts {
  /** providerId -> viewers summed over that platform's accounts. A platform none of
   * whose accounts reported is ABSENT rather than present at 0, so a widget cannot put
   * a live zero on stream for a platform that said nothing. */
  perPlatform: Record<string, number>;
}

type LoadCtx = { fields: Record<string, unknown> };
type LoadHandler = (ctx: LoadCtx) => void;
type EventHandler = (e: NormalizedEvent) => void;
type ChatHandler = (m: ChatMessage) => void;
type ViewersHandler = (v: ViewerSnapshot) => void;

const boot: OverlayBootstrap = (window as unknown as { __OVERLAY__: OverlayBootstrap }).__OVERLAY__ ?? {
  id: "",
  token: "",
  port: 43000,
  fields: {},
};

const loadHandlers: LoadHandler[] = [];
const eventHandlers: EventHandler[] = [];
const chatHandlers: ChatHandler[] = [];
const viewersHandlers: ViewersHandler[] = [];

const OBSOverlay = {
  fields: boot.fields,
  onLoad(fn: LoadHandler) {
    loadHandlers.push(fn);
  },
  onEvent(fn: EventHandler) {
    eventHandlers.push(fn);
  },
  onChat(fn: ChatHandler) {
    chatHandlers.push(fn);
  },
  onViewers(fn: ViewersHandler) {
    viewersHandlers.push(fn);
  },
  playSound(url: string, volume = 1) {
    if (!url) return;
    const a = new Audio(url);
    a.volume = Math.max(0, Math.min(1, volume));
    void a.play().catch(() => {});
  },
};

(window as unknown as { OBSOverlay: typeof OBSOverlay }).OBSOverlay = OBSOverlay;

function fireLoad() {
  const ctx: LoadCtx = { fields: boot.fields };
  for (const fn of loadHandlers) {
    try {
      fn(ctx);
    } catch (e) {
      console.log("OBSOverlay onLoad threw: " + (e as Error).message);
    }
  }
  window.dispatchEvent(new CustomEvent("obs:load", { detail: ctx }));
}

function fireEvent(e: NormalizedEvent) {
  for (const fn of eventHandlers) {
    try {
      fn(e);
    } catch (err) {
      console.log("OBSOverlay onEvent threw: " + (err as Error).message);
    }
  }
  window.dispatchEvent(new CustomEvent("obs:event", { detail: e }));
}

function fireChat(m: ChatMessage) {
  for (const fn of chatHandlers) {
    try {
      fn(m);
    } catch (err) {
      console.log("OBSOverlay onChat threw: " + (err as Error).message);
    }
  }
  window.dispatchEvent(new CustomEvent("obs:chat", { detail: m }));
}

// The per-platform sum lives here rather than in each template.js: the host payload is
// keyed per account, so every widget wanting platform chips would otherwise repeat this
// split and they would drift apart. Same derivation the app's viewerCountStore does for
// its own bundle -- an account key is the host's OAuth::AccountId, "<providerId>:<userId>".
function toSnapshot(counts: ViewerCounts): ViewerSnapshot {
  const perPlatform: Record<string, number> = {};
  for (const [accountId, n] of Object.entries(counts.perAccount ?? {})) {
    const providerId = accountId.split(":")[0];
    perPlatform[providerId] = (perPlatform[providerId] ?? 0) + n;
  }
  return { ...counts, perPlatform };
}

function fireViewers(counts: ViewerCounts) {
  const v = toSnapshot(counts);
  for (const fn of viewersHandlers) {
    try {
      fn(v);
    } catch (err) {
      console.log("OBSOverlay onViewers threw: " + (err as Error).message);
    }
  }
  window.dispatchEvent(new CustomEvent("obs:viewers", { detail: v }));
}

// EventSource auto-reconnects on drop; the host keepalive keeps it warm.
const src = new EventSource("/w/" + boot.id + "/events?t=" + boot.token);
src.onmessage = (msg) => {
  try {
    fireEvent(JSON.parse(msg.data) as NormalizedEvent);
  } catch {
    /* ignore a malformed frame */
  }
};
// Chat rides a NAMED SSE event, so it bypasses onmessage entirely -- alert-box
// widgets that never call onChat are unaffected.
src.addEventListener("chat", (msg) => {
  try {
    fireChat(JSON.parse((msg as MessageEvent).data) as ChatMessage);
  } catch {
    /* ignore a malformed frame */
  }
});
// Viewer counts ride their own named event for the same reason. The poller pushes only
// while live and never sends a closing zero, so a widget keeps the last cycle on screen
// until the source reloads -- it must not invent a 0 of its own.
src.addEventListener("viewers", (msg) => {
  try {
    fireViewers(JSON.parse((msg as MessageEvent).data) as ViewerCounts);
  } catch {
    /* ignore a malformed frame */
  }
});

// Fire load once the DOM + handlers are ready. Handlers registered synchronously in
// the user JS run before this microtask, so a raf defer is enough.
if (document.readyState === "complete" || document.readyState === "interactive") {
  requestAnimationFrame(fireLoad);
} else {
  window.addEventListener("DOMContentLoaded", () => requestAnimationFrame(fireLoad));
}

export {}; // isolatedModules module marker; --format iife strips this from the built output
