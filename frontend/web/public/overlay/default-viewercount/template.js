const totalEl = document.getElementById("total");
const chipsEl = document.getElementById("chips");

// Per-platform label + accent + the field keys that override them. One row per
// platform: adding a fourth is a row here, not a new branch. Order mirrors the app's
// PLATFORM_ORDER so a chip row never reshuffles between cycles.
const PLATFORMS = [
  { id: "twitch", label: "Twitch", showKey: "showTwitch", colorKey: "colorTwitch", color: "#a970ff" },
  { id: "youtube", label: "YouTube", showKey: "showYouTube", colorKey: "colorYouTube", color: "#ff4e45" },
  { id: "kick", label: "Kick", showKey: "showKick", colorKey: "colorKick", color: "#53fc18" },
];

// The align choice as both a text-align keyword and its flex equivalent.
const ALIGN = {
  left: { text: "left", flex: "flex-start" },
  center: { text: "center", flex: "center" },
  right: { text: "right", flex: "flex-end" },
};

let fields = {};
// The last cycle the poller pushed, or null when none has arrived. The poller only runs
// while live, so null means "nothing has reported" -- it is not zero and must render as
// nothing. Fields can be re-applied at any time, so every render reads this snapshot
// instead of assuming a cycle follows.
let snapshot = null;

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onViewers((v) => {
  snapshot = v;
  render();
});
// The poller stops with the broadcast and never sends a closing cycle, so the last counts
// would otherwise stay on the canvas for the rest of the scene. Clearing back to null is
// the same "nothing has reported" state the widget starts in and already draws as nothing;
// a fabricated zero would put a false figure in front of an audience instead.
OBSOverlay.onStream((s) => {
  if (s && s.active !== true) {
    snapshot = null;
    render();
  }
});

function applyFields(f) {
  fields = f;
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  if (f.fontSize != null) set("--ov-size", (Number(f.fontSize) || 24) + "px");
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  if (f.gap != null) set("--ov-gap", Math.max(0, Number(f.gap) || 0) + "px");

  const column = f.orientation === "column";
  const align = ALIGN[String(f.align || "left")] || ALIGN.left;
  set("--ov-dir", column ? "column" : "row");
  set("--ov-align", align.text);
  // In a row the chips want vertical centering; in a column the align choice becomes
  // the cross axis, so it drives align-items instead of only text-align.
  set("--ov-cross", column ? align.flex : "center");

  render();
}

// Checkbox reader with a per-field fallback for when the key is missing entirely (an
// older widget, or the user deleted the field). One predicate rather than a truthiness
// test per call site, so the platform toggles cannot drift apart from each other.
function isOn(key, fallback) {
  const v = fields[key];
  if (v == null) {
    return fallback;
  }
  return v === true || v === "true";
}

// A platform that did not report is ABSENT from perPlatform, never present at 0 --
// "not connected", "not live", "cannot report" and "read errored" all look like absence,
// while a genuine 0 is present with value 0. Presence is therefore the only admissible
// test: `if (perPlatform[id])` would swallow the real zero, and inventing a zero would
// put a false "0 viewers" on a live stream. null here means absent, never zero.
function reportedCount(id) {
  const per = snapshot.perPlatform || {};
  if (!Object.prototype.hasOwnProperty.call(per, id)) {
    return null;
  }
  const n = Number(per[id]);
  return Number.isFinite(n) ? n : null;
}

function idleText() {
  return fields.idleText != null ? String(fields.idleText) : "—";
}

function accentFor(p) {
  return fields[p.colorKey] ? String(fields[p.colorKey]) : p.color;
}

// Every value lands via textContent, so a platform label or a user format string cannot
// inject markup.
function makeChip(p, n) {
  const chip = document.createElement("span");
  chip.className = n === null ? "chip idle" : "chip";

  if (isOn("showIcons", true)) {
    const dot = document.createElement("span");
    dot.className = "dot";
    dot.style.background = accentFor(p);
    chip.appendChild(dot);
  }
  if (isOn("showPlatformLabels", false)) {
    const label = document.createElement("span");
    label.className = "plat";
    label.textContent = p.label;
    chip.appendChild(label);
  }

  const count = document.createElement("span");
  count.className = "count";
  count.textContent = n === null ? idleText() : n.toLocaleString();
  chip.appendChild(count);

  return chip;
}

function blank() {
  chipsEl.textContent = "";
  chipsEl.hidden = true;
  totalEl.textContent = "";
  totalEl.hidden = true;
}

function render() {
  if (snapshot === null) {
    blank();
    return;
  }

  const mode = String(fields.mode || "separated");
  const wantTotal = mode === "aggregate" || mode === "both";
  const wantChips = mode !== "aggregate";
  const showIdle = isOn("showIdle", false);

  chipsEl.textContent = "";
  let sum = 0;
  let reporting = 0;

  for (const p of PLATFORMS) {
    if (!isOn(p.showKey, true)) {
      continue;
    }
    const n = reportedCount(p.id);
    if (n !== null) {
      sum += n;
      reporting += 1;
    }
    if (!wantChips) {
      continue;
    }
    // An absent platform renders nothing at all unless the user asked for the idle
    // placeholder -- and even then it shows idleText, never a number.
    if (n === null && !showIdle) {
      continue;
    }
    chipsEl.appendChild(makeChip(p, n));
  }

  chipsEl.hidden = !wantChips || chipsEl.childElementCount === 0;

  // The host's `total` sums every account, including platforms the user deselected, so
  // printing it beside the chips could contradict them. The aggregate is the sum of what
  // is actually shown; with nothing deselected the two agree by construction.
  totalEl.textContent = wantTotal && reporting > 0 ? formatTotal(sum) : "";
  totalEl.hidden = totalEl.textContent.length === 0;
}

function formatTotal(sum) {
  const tmpl = fields.totalFormat != null ? String(fields.totalFormat) : "{count} viewers";
  return tmpl.replaceAll("{count}", sum.toLocaleString());
}
