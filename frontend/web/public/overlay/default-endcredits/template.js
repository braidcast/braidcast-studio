const creditsEl = document.getElementById("credits");
const rollEl = document.getElementById("roll");
const titleEl = document.getElementById("title");
const sectionsEl = document.getElementById("sections");
const emptyEl = document.getElementById("empty");

// The sections in roll order: the field that shows each, its heading, and the event types
// whose actors it thanks. Registry list, not a switch: a new section is one row here and a
// field in fields.json.
const SECTIONS = [
  { field: "showSubs", heading: "Subscribers", types: ["sub", "resub", "subgift"] },
  { field: "showMembers", heading: "Members", types: ["member"] },
  { field: "showSuperChats", heading: "Super Chats", types: ["superchat", "supersticker"] },
  { field: "showCheers", heading: "Cheers", types: ["cheer"] },
  { field: "showKicks", heading: "Kicks", types: ["kicks"] },
  { field: "showRaids", heading: "Raiders", types: ["raid"] },
  { field: "showFollows", heading: "New followers", types: ["follow"] },
];

let fields = {};
// This broadcast's events as this page knows them, by id: the backfill on connect, then
// live ones, each counted once. A test frame is kept only in the editor preview, until the
// next backfill. The host's backfill is capped, so a very busy broadcast's earliest
// supporters can be missing after a reload.
let events = [];
const ids = new Set();

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onBackfill((list) => {
  events = [];
  ids.clear();
  for (const e of list) remember(e);
  render();
});
OBSOverlay.onEvent((e) => {
  if (!e || e.replay) return;
  if (e.test && !OBSOverlay.preview) return;
  if (remember(e)) render();
});

function remember(e) {
  if (!e || !e.id || ids.has(e.id)) return false;
  ids.add(e.id);
  events.push(e);
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
  if (f.fontSize != null) set("--ov-size", String(Number(f.fontSize) || 28));
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.headingColor) set("--ov-heading", String(f.headingColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  render();
  restart();
}

// Each shown section with the people it thanks: unique names in the order they first
// appear, so someone who subbed twice is thanked once.
function sections() {
  const out = [];
  for (const s of SECTIONS) {
    if (!isOn(s.field, true)) continue;
    const names = [];
    const seen = new Set();
    for (const e of events) {
      if (s.types.indexOf(e.type) === -1) continue;
      const name = e.actorName || "";
      const key = name.toLowerCase();
      if (!name || seen.has(key)) continue;
      seen.add(key);
      names.push(name);
    }
    if (names.length > 0) out.push({ heading: s.heading, names });
  }
  return out;
}

// Every value lands via textContent, so a viewer's name cannot inject markup.
function render() {
  titleEl.textContent = fields.title != null ? String(fields.title) : "Thanks for watching!";
  titleEl.hidden = titleEl.textContent === "";
  sectionsEl.textContent = "";
  const list = sections();
  for (const s of list) {
    const section = document.createElement("section");
    section.className = "section";
    const h = document.createElement("h2");
    h.textContent = s.heading;
    section.appendChild(h);
    const ul = document.createElement("ul");
    for (const name of s.names) {
      const li = document.createElement("li");
      li.textContent = name;
      ul.appendChild(li);
    }
    section.appendChild(ul);
    sectionsEl.appendChild(section);
  }
  const empty = fields.emptyText != null ? String(fields.emptyText) : "";
  emptyEl.textContent = empty;
  emptyEl.hidden = list.length > 0 || empty === "";
}

// --- the roll ------------------------------------------------------------------------
// Moved by hand each frame rather than by a fixed-length animation, because the roll grows
// while it runs: a name added mid-roll lengthens it instead of restarting it. The offset is
// in device px; the speed field is in design px, resolved against the root scale as the
// ticker resolves its belt speed.
const reducedMotion = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
let offset = 0;
let last = 0;
let done = false;

function designPx() {
  const root = parseFloat(getComputedStyle(document.documentElement).fontSize) || 16;
  return root / 16;
}

// From just below the frame; with reduced motion, standing at the top.
function restart() {
  offset = reducedMotion ? 0 : creditsEl.clientHeight;
  done = false;
  place();
}

function place() {
  rollEl.style.transform = "translateY(" + offset.toFixed(1) + "px)";
}

function frame(now) {
  const dt = last ? Math.min(0.1, (now - last) / 1000) : 0;
  last = now;
  if (!reducedMotion && !done) {
    const speed = Math.max(10, Math.min(200, Number(fields.scrollSpeed) || 50));
    offset -= speed * designPx() * dt;
    if (offset < -rollEl.offsetHeight) {
      if (isOn("loop", true)) {
        offset = creditsEl.clientHeight;
      } else {
        // Hold on the end: the last section sits at the bottom of the frame.
        offset = creditsEl.clientHeight - rollEl.offsetHeight;
        done = true;
      }
    }
    place();
  }
  requestAnimationFrame(frame);
}

restart();
if (!reducedMotion) {
  requestAnimationFrame(frame);
}
