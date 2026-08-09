// The built-in overlay widget types, one row each. The "New overlay" menu reads `label`
// and `name`; the preview's test bar reads `tests`. Both read this one table, so adding a
// widget type is a row here and nothing else.
//
// `tests` is not a judgement about what a type "should" accept — it is the set of SSE
// channels that type's shipped public/overlay/default-<type>/template.js actually
// subscribes to. A control the template has no handler for would paint nothing, which is
// how a chat box came to show Follow and Sub buttons.

/** What the preview can feed a widget.
 *  - `alerts`   — template calls OBSOverlay.onEvent (the default SSE channel)
 *  - `chat`     — onChat
 *  - `viewers`  — onViewers
 *  - `channels` — onChannelStats
 *  - `stream`   — onStream, and renders from the broadcast state (uptime, destinations)
 *  - `clear`    — onStream, but only to reset itself when a broadcast ends; the widget
 *                 draws nothing from the frame, so the only control it earns is the reset
 */
export type TestCapability = "alerts" | "chat" | "viewers" | "channels" | "stream" | "clear";

export interface WidgetTypeSpec {
  /** Backend widget type, and the public/overlay/default-<type>/ template directory. */
  type: string;
  label: string;
  /** Name a freshly created widget of this type gets. */
  name: string;
  tests: TestCapability[];
}

export const WIDGET_TYPES: WidgetTypeSpec[] = [
  { type: "alertbox", label: "Alert Box", name: "New Alert Box", tests: ["alerts"] },
  { type: "chatbox", label: "Chat Box", name: "New Chat Box", tests: ["chat", "clear"] },
  { type: "ticker", label: "Event Ticker", name: "New Event Ticker", tests: ["alerts"] },
  { type: "goalbar", label: "Goal Bar", name: "New Goal Bar", tests: ["alerts"] },
  { type: "labels", label: "Label", name: "New Label", tests: ["alerts"] },
  { type: "viewercount", label: "Viewer Count", name: "New Viewer Count", tests: ["viewers", "clear"] },
  { type: "followercount", label: "Follower Count", name: "New Follower Count", tests: ["channels"] },
  { type: "uptime", label: "Stream Uptime", name: "New Stream Uptime", tests: ["stream"] },
  { type: "wheretowatch", label: "Where to Watch", name: "New Where to Watch", tests: ["stream"] },
  { type: "chatleaderboard", label: "Chat Leaderboard", name: "New Chat Leaderboard", tests: ["chat", "clear"] },
];

const BY_TYPE = new Map(WIDGET_TYPES.map((w) => [w.type, w]));

/** A widget's test capabilities. Empty for a type this build doesn't ship — a document
 * written by a newer version, or a hand-edited one — so an unknown type offers no
 * controls rather than the wrong ones. */
export function testsFor(type: string): TestCapability[] {
  return BY_TYPE.get(type)?.tests ?? [];
}
