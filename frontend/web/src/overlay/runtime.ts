// The clean native widget API injected into every served overlay document. Runs
// inside an OBS Browser Source (its own CEF process), NOT the app. Reads the
// host-injected window.__OVERLAY__ = {id, token, port, fields} and streams
// NormalizedEvents over SSE. Compiled to dist/overlay/runtime.js by bun build.

// ChatHub fans ONE payload object to both the overlay server and the bridge, so the
// wire shape here is the bridge's shape. Type-only, so it erases at build time and the
// runtime still bundles standalone (no bridge module pulled in).
import type { AudienceKind, ChannelStats, ChatMessage, NormalizedEvent, ViewerCounts } from "$lib/api/bridge";

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

/** One platform's audience total, grouped from the per-account entries. Never a plain
 * number: a platform can withhold the figure, and a withheld figure must not be able to
 * reach a widget as a 0. */
export interface AudienceGroup {
  /** Summed over this platform's accounts that reported a real figure, or `null` when
   * NONE did. Null and 0 are different answers -- null means "nobody would tell us". */
  count: number | null;
  /** What `count` counts. A property of the platform rather than the channel, so it is
   * the same across a provider's accounts and summing within a provider stays meaningful
   * (summing ACROSS providers would not, which is why no grand total is derived). */
  kind: AudienceKind;
  /** Accounts of this platform present in the payload, i.e. read at least once. */
  accounts: number;
  /** How many of those contributed to `count`. `accounts - counted` were withheld or
   * unknown, so a widget can render "3 of 4 channels" rather than a wrong total. */
  counted: number;
  /** At least one of this platform's accounts has the figure hidden by its owner. With
   * `counted < accounts` and `hidden` false, the rest were simply never read. */
  hidden: boolean;
}

/** A channel-stats cycle as a widget sees it: the host payload verbatim, plus the
 * per-platform grouping every widget would otherwise derive for itself. */
export interface ChannelStatsSnapshot extends ChannelStats {
  /** providerId -> that platform's audience group. A platform with no account in the
   * payload is ABSENT rather than present at 0. */
  perPlatform: Record<string, AudienceGroup>;
}

type LoadCtx = { fields: Record<string, unknown> };
type LoadHandler = (ctx: LoadCtx) => void;
type EventHandler = (e: NormalizedEvent) => void;
type ChatHandler = (m: ChatMessage) => void;
type ViewersHandler = (v: ViewerSnapshot) => void;
type ChannelStatsHandler = (s: ChannelStatsSnapshot) => void;

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
const channelStatsHandlers: ChannelStatsHandler[] = [];

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
  onChannelStats(fn: ChannelStatsHandler) {
    channelStatsHandlers.push(fn);
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

// Grouped per platform, never summed across platforms: a Twitch follower count and a
// YouTube subscriber count measure different things, so their sum names nothing. Within a
// provider the kind is fixed, so that sum is meaningful. An account whose figure is hidden
// or unknown (-1) is excluded from `count` yet still counted in `accounts`, so "withheld"
// stays distinguishable from "never read" and can never collapse into a number. Account
// keys are the host's OAuth::AccountId, "<providerId>:<userId>".
function toChannelSnapshot(stats: ChannelStats): ChannelStatsSnapshot {
  const perPlatform: Record<string, AudienceGroup> = {};
  for (const [accountId, e] of Object.entries(stats.perAccount ?? {})) {
    const providerId = accountId.split(":")[0];
    const g = (perPlatform[providerId] ??= { count: null, kind: "", accounts: 0, counted: 0, hidden: false });
    g.accounts += 1;
    if (e.audienceHidden) g.hidden = true;
    if (e.audienceKind && !g.kind) g.kind = e.audienceKind;
    if (!e.audienceHidden && typeof e.audienceCount === "number" && e.audienceCount >= 0) {
      g.count = (g.count ?? 0) + e.audienceCount;
      g.counted += 1;
    }
  }
  return { ...stats, perPlatform };
}

function fireChannelStats(stats: ChannelStats) {
  const s = toChannelSnapshot(stats);
  for (const fn of channelStatsHandlers) {
    try {
      fn(s);
    } catch (err) {
      console.log("OBSOverlay onChannelStats threw: " + (err as Error).message);
    }
  }
  window.dispatchEvent(new CustomEvent("obs:channelstats", { detail: s }));
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
// Audience totals ride their own named event. The poller is always-on rather than
// go-live-gated, so a widget gets ticks off-stream too -- but the cadence is ~15 minutes,
// so the first frame can be that far out from a source reload.
src.addEventListener("channels", (msg) => {
  try {
    fireChannelStats(JSON.parse((msg as MessageEvent).data) as ChannelStats);
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
