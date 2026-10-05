const rowsEl = document.getElementById("rows");
const emptyEl = document.getElementById("empty");

// Which event types each "show" field covers. Registry map, not a switch: a new type is a
// row here and a field in fields.json.
const SHOWS = {
  showFollows: ["follow"],
  showSubs: ["sub", "resub", "subgift"],
  showMembers: ["member"],
  showCheers: ["cheer"],
  showSuperChats: ["superchat", "supersticker"],
  showKicks: ["kicks"],
  showRaids: ["raid"],
};

// Per-platform accent for the row's edge (the app's Events/Multichat palette).
const PLATFORM_COLOR = { twitch: "#a970ff", youtube: "#ff4e45", kick: "#53fc18" };

const ALIGN = { left: "flex-start", center: "center", right: "flex-end" };

let fields = {};
let shown = new Set(Object.values(SHOWS).flat());
// This broadcast's events as this page knows them, oldest first, by id: the backfill on
// connect, then live ones. One event can arrive in both, and is listed once. A test frame
// is kept only in the editor preview, until the next backfill.
let events = [];
const ids = new Set();

const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onBackfill((list) => {
  events = [];
  ids.clear();
  for (const e of list) remember(e);
  render(false);
});
OBSOverlay.onEvent((e) => {
  // A replay is a second showing of an event already listed.
  if (!e || e.replay) return;
  if (e.test && !OBSOverlay.preview) return;
  if (remember(e)) render(true);
});

function remember(e) {
  if (!e || !e.id || ids.has(e.id)) return false;
  ids.add(e.id);
  events.push(e);
  // Hold a little more than any setting shows, so a filter change has rows to draw.
  if (events.length > 100) ids.delete(events.shift().id);
  return true;
}

function isOn(key, fallback) {
  const v = fields[key];
  if (v == null) return fallback;
  return v === true || v === "true";
}

function applyFields(f) {
  fields = f;
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  // Design px, not device px: template.css resolves it against the root scale.
  if (f.fontSize != null) set("--ov-size", String(Number(f.fontSize) || 20));
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.nameColor) set("--ov-name", String(f.nameColor));
  if (f.rowColor) set("--ov-row", String(f.rowColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  const align = own(ALIGN, f.align) ? f.align : "left";
  set("--ov-align", align);
  set("--ov-cross", ALIGN[align]);

  shown = new Set();
  for (const key of Object.keys(SHOWS)) {
    if (isOn(key, true)) for (const t of SHOWS[key]) shown.add(t);
  }
  render(false);
}

// Every value lands via textContent, so a viewer's name cannot inject markup.
function makeRow(e, animate) {
  const row = document.createElement("li");
  row.className = "row";
  if (!animate) row.style.animation = "none";
  if (isOn("showPlatform", true) && own(PLATFORM_COLOR, e.platform)) {
    row.style.setProperty("--ov-platform", PLATFORM_COLOR[e.platform]);
  }
  const emoji = OBSOverlay.eventEmoji(String(e.type || ""));
  if (isOn("showEmoji", true) && emoji) {
    const el = document.createElement("span");
    el.className = "emoji";
    el.textContent = emoji;
    row.appendChild(el);
  }
  const name = document.createElement("span");
  name.className = "name";
  name.textContent = e.actorName || "Someone";
  row.appendChild(name);
  const what = document.createElement("span");
  what.className = "what";
  what.textContent = OBSOverlay.summarize(e);
  row.appendChild(what);
  return row;
}

// `animate` is true only for the one event that just arrived; a rebuild (a backfill, a
// settings change) redraws without every row sliding in again.
function render(animate) {
  const max = Math.max(1, Math.min(20, Math.round(Number(fields.maxItems) || 6)));
  const newestOnTop = isOn("newestOnTop", true);
  const picked = events.filter((e) => shown.has(e.type)).slice(-max);
  if (newestOnTop) picked.reverse();
  rowsEl.classList.toggle("oldest-first", !newestOnTop);
  rowsEl.textContent = "";
  const newest = picked.length > 0 ? (newestOnTop ? picked[0] : picked[picked.length - 1]) : null;
  for (const e of picked) rowsEl.appendChild(makeRow(e, animate && e === newest));
  const empty = fields.emptyText != null ? String(fields.emptyText) : "";
  emptyEl.textContent = empty;
  emptyEl.hidden = picked.length > 0 || empty === "";
}
