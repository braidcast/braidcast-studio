const titleEl = document.getElementById("title");
const chipsEl = document.getElementById("chips");

// Per-platform label + accent + the field key that overrides it, keyed by the platform
// string the host sends. Adding a platform is a row here, not a new branch.
const PLATFORMS = {
  twitch: { label: "Twitch", colorKey: "colorTwitch", color: "#a970ff" },
  youtube: { label: "YouTube", colorKey: "colorYouTube", color: "#ff4e45" },
  kick: { label: "Kick", colorKey: "colorKick", color: "#53fc18" },
};

// Every key the table has no row for. A stream profile with no linked account carries its
// service word rather than a provider id ("rtmp", "whip", "custom"), and none of those has
// a brand colour -- so the fallback is an explicit entry instead of a lookup that can
// return undefined. `label` is null because only the key itself names such a destination;
// it is upper-cased at use because every key that lands here is a protocol word.
const OTHER = { label: null, colorKey: "colorOther", color: "#9aa0a6" };

// Stable chip order, mirroring the app's PLATFORM_ORDER. Destinations arrive in the
// engine's order, which changes as outputs connect, drop and reconnect, so an unsorted row
// would reshuffle under a viewer mid-broadcast.
const PLATFORM_ORDER = ["twitch", "youtube", "kick"];

// The align choice as both a text-align keyword and its flex equivalent.
const ALIGN = {
  left: { text: "left", flex: "flex-start" },
  center: { text: "center", flex: "center" },
  right: { text: "right", flex: "flex-end" },
};

let fields = {};
// The last broadcast state, or null before the first frame. The host replays `stream` on
// connect, so a source added mid-broadcast lists the destinations at once.
let state = null;

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onStream((s) => {
  state = s;
  render();
});

function applyFields(f) {
  fields = f;
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  if (f.fontSize != null) set("--ov-size", (Number(f.fontSize) || 24) + "px");
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  if (f.gap != null) set("--ov-gap", Math.max(0, Number(f.gap) || 0) + "px");

  const column = String(f.orientation || "column") === "column";
  const align = ALIGN[String(f.align || "left")] || ALIGN.left;
  set("--ov-dir", column ? "column" : "row");
  set("--ov-align", align.text);
  // In a row the chips want vertical centering; in a column the align choice becomes the
  // cross axis, so it drives align-items instead of only text-align.
  set("--ov-cross", column ? align.flex : "center");

  render();
}

// Checkbox reader with a per-field fallback for when the key is missing entirely (an
// older widget, or the user deleted the field). One predicate rather than a truthiness
// test per call site, so the toggles cannot drift apart from each other.
function isOn(key, fallback) {
  const v = fields[key];
  if (v == null) {
    return fallback;
  }
  return v === true || v === "true";
}

// An empty string the user typed deliberately is a valid answer, so only a missing key
// falls back.
function textField(key, fallback) {
  const v = fields[key];
  return v != null ? String(v) : fallback;
}

function platformFor(id) {
  return Object.prototype.hasOwnProperty.call(PLATFORMS, id) ? PLATFORMS[id] : OTHER;
}

function labelFor(id) {
  const p = platformFor(id);
  return p.label !== null ? p.label : id.toUpperCase();
}

function accentFor(id) {
  const p = platformFor(id);
  return fields[p.colorKey] ? String(fields[p.colorKey]) : p.color;
}

function orderOf(id) {
  const i = PLATFORM_ORDER.indexOf(id);
  return i === -1 ? PLATFORM_ORDER.length : i;
}

// The one place a destination's word is decided, so render() holds no absence logic.
// `name` is the profile's raw label and may be null or blank, in which case the platform's
// own word stands in -- a viewer is never shown a chip with nothing in it. null means the
// destination names nothing at all, which is a row carrying neither a label nor a platform
// key, and it draws nothing.
function rowFor(d) {
  if (!d || typeof d !== "object") {
    return null;
  }
  const platform = typeof d.platform === "string" ? d.platform.trim().toLowerCase() : "";
  const label = platform ? labelFor(platform) : "";
  const raw = typeof d.name === "string" ? d.name.trim() : "";
  const text = String(fields.nameSource || "channel") === "platform" ? label : raw || label;
  if (!text) {
    return null;
  }
  return { uuid: typeof d.bindingUuid === "string" ? d.bindingUuid : "", platform, text };
}

// Every value lands via textContent, so a profile label the user typed cannot inject
// markup.
function makeChip(row) {
  const chip = document.createElement("span");
  chip.className = "chip";

  if (isOn("showIcons", true)) {
    const dot = document.createElement("span");
    dot.className = "dot";
    dot.style.background = accentFor(row.platform);
    chip.appendChild(dot);
  }

  const name = document.createElement("span");
  name.className = "name";
  name.textContent = row.text;
  chip.appendChild(name);

  return chip;
}

function blank() {
  titleEl.textContent = "";
  titleEl.hidden = true;
  chipsEl.textContent = "";
  chipsEl.hidden = true;
}

function render() {
  // `active` comes from the engine rather than from destinations being non-empty: an
  // output live under a binding disabled mid-broadcast is live yet enumerates nowhere, and
  // that is not the same state as off air.
  if (state === null || (isOn("hideWhenOffline", true) && state.active !== true)) {
    blank();
    return;
  }

  const rows = [];
  const seenBinding = new Set();
  const seenText = new Set();
  for (const d of Array.isArray(state.destinations) ? state.destinations : []) {
    const row = rowFor(d);
    if (row === null) {
      continue;
    }
    // One binding is one destination however many rows describe it. The second guard is
    // about the reading rather than about identity: two chips carrying the same word tell
    // a viewer the same thing twice, which is what two unnamed profiles on one platform --
    // or the platform name source with two channels there -- would otherwise produce.
    if (row.uuid && seenBinding.has(row.uuid)) {
      continue;
    }
    if (seenText.has(row.text)) {
      continue;
    }
    if (row.uuid) {
      seenBinding.add(row.uuid);
    }
    seenText.add(row.text);
    rows.push(row);
  }

  // Known platforms in the app's order, everything else after in one bucket, then by the
  // word on the chip -- which is unique by the dedupe above, so the order is total.
  rows.sort((a, b) => orderOf(a.platform) - orderOf(b.platform) || a.text.localeCompare(b.text));

  chipsEl.textContent = "";
  for (const row of rows) {
    chipsEl.appendChild(makeChip(row));
  }
  chipsEl.hidden = chipsEl.childElementCount === 0;

  // A heading over an empty list introduces nothing, so it goes down with the chips.
  const title = textField("title", "Also streaming on");
  const wantTitle = isOn("showTitle", true) && title.length > 0 && !chipsEl.hidden;
  titleEl.textContent = wantTitle ? title : "";
  titleEl.hidden = !wantTitle;
}
