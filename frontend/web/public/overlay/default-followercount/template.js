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
// The last cycle the poller pushed, or null when none has arrived. The poller is
// always-on rather than go-live-gated, but its cadence is ~15 minutes, so null can
// outlast the source's load by that long and must still render as nothing -- there is
// no figure to stand in for. Fields can be re-applied at any time, so every render
// reads this snapshot instead of assuming a cycle follows.
let snapshot = null;

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onChannelStats((s) => {
  snapshot = s;
  render();
});

function applyFields(f) {
  fields = f;
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  // Design px, not device px: template.css resolves it against the root scale.
  if (f.fontSize != null) set("--ov-size", String(Number(f.fontSize) || 24));
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  if (f.gap != null) set("--ov-gap", String(Math.max(0, Number(f.gap) || 0)));

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

// Same shape for the placeholder strings: an empty string the user typed deliberately
// is a valid answer, so only a missing key falls back.
function textField(key, fallback) {
  const v = fields[key];
  return v != null ? String(v) : fallback;
}

// A platform with no account in the payload is ABSENT from perPlatform, never present
// at a zeroed group. Presence is therefore the only admissible test: `if (per[id])`
// would be true for every group that exists, but it also cannot distinguish the absent
// platform from one whose count is a real 0. The runtime owns the per-account grouping
// so widgets cannot drift from it -- nothing here re-derives from perAccount.
function groupFor(id) {
  const per = snapshot.perPlatform || {};
  if (!Object.prototype.hasOwnProperty.call(per, id)) {
    return null;
  }
  const g = per[id];
  return g && typeof g === "object" ? g : null;
}

function accentFor(p) {
  return fields[p.colorKey] ? String(fields[p.colorKey]) : p.color;
}

// Twitch counts followers and YouTube counts subscribers; they are different things, so
// the platform's own word comes from the payload rather than from a constant here. An
// empty kind (no account reported one) would print a bare number with a stray gap, so
// the platform name stands in.
function labelFor(p, group) {
  const mode = String(fields.labelMode || "kind");
  if (mode === "none") {
    return "";
  }
  if (mode === "platform") {
    return p.label;
  }
  const kind = group ? String(group.kind || "") : "";
  return kind || p.label;
}

// Disclosed only alongside a real figure: it qualifies that figure as a sum over some of
// the platform's channels, and beside a placeholder it would qualify nothing.
function partialFor(group) {
  if (!isOn("showPartial", false) || group.counted >= group.accounts) {
    return "";
  }
  return group.counted + "/" + group.accounts;
}

// The one place the four no-figure states are told apart, so no caller can invent a
// fifth. Returns null when the platform draws nothing at all; otherwise what its chip
// reads, which is an audience figure only in the last case.
function rowFor(p) {
  const group = groupFor(p.id);

  // Absent: no account of this platform is in the payload, so there is nothing known
  // about it -- not a zero, and not a withheld figure either.
  if (group === null) {
    return isOn("showIdle", false) ? { group: null, text: textField("idleText", "—"), partial: "", muted: true } : null;
  }

  const count = typeof group.count === "number" ? group.count : null;
  if (count === null) {
    // A withheld figure is a fact rather than a failure: the channel is there and its
    // owner chose not to publish the number (YouTube subscriber counts commonly are), so
    // it gets a mark of its own instead of the silence an unread platform gets. `hidden`
    // is the only thing separating the two.
    if (group.hidden) {
      return { group, text: textField("hiddenText", "—"), partial: "", muted: true };
    }
    return isOn("showIdle", false) ? { group, text: textField("idleText", "—"), partial: "", muted: true } : null;
  }

  // A real figure, including a genuine 0 -- which is why every test above is against
  // null and not truthiness. It sums the accounts that reported; when others withheld or
  // were never read it stays true but incomplete, which the opt-in marker discloses.
  return { group, text: count.toLocaleString(), partial: partialFor(group), muted: false };
}

// Every value lands via textContent, so a platform's own word or a user's placeholder
// string cannot inject markup.
function makeChip(p, row) {
  const chip = document.createElement("span");
  chip.className = row.muted ? "chip muted" : "chip";

  if (isOn("showIcons", true)) {
    const dot = document.createElement("span");
    dot.className = "dot";
    dot.style.background = accentFor(p);
    chip.appendChild(dot);
  }

  const count = document.createElement("span");
  count.className = "count";
  count.textContent = row.text;
  chip.appendChild(count);

  const label = labelFor(p, row.group);
  if (label) {
    const el = document.createElement("span");
    el.className = "plat";
    el.textContent = label;
    chip.appendChild(el);
  }
  if (row.partial) {
    const el = document.createElement("span");
    el.className = "partial";
    el.textContent = row.partial;
    chip.appendChild(el);
  }

  return chip;
}

function blank() {
  chipsEl.textContent = "";
  chipsEl.hidden = true;
}

function render() {
  if (snapshot === null) {
    blank();
    return;
  }

  chipsEl.textContent = "";
  for (const p of PLATFORMS) {
    if (!isOn(p.showKey, true)) {
      continue;
    }
    const row = rowFor(p);
    if (row !== null) {
      chipsEl.appendChild(makeChip(p, row));
    }
  }
  chipsEl.hidden = chipsEl.childElementCount === 0;
}
