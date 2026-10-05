const textEl = document.getElementById("label-text");

// The stored eventType -> the Counter source whose event types it shows, so which events a
// label, a goal bar and a Counter count is decided in one place (OBSOverlay.counter). The
// stored keys stay as they are; only the lookup moved. "subscriber" counts resubs too, so a
// running total reflects every sub event.
const EVENTS = {
  follower: "follow",
  subscriber: "sub",
  giftedsub: "subgift",
  cheer: "cheer",
  kicks: "kicks",
  raid: "raid",
  superchat: "superchat",
};
const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
const typesOf = (eventType) => OBSOverlay.counter.sources[own(EVENTS, eventType) ? EVENTS[eventType] : EVENTS.follower].types;

let types = typesOf("follower");
let mode = "latest";
let format = "Latest follower: {name}";
let emptyText = "—";
let count = 0;
let lastName = "";
let lastAmount = "";
let hasData = false;

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onEvent((e) => {
  if (!e || types.indexOf(e.type) === -1) return;
  count += 1;
  lastName = e.actorName || "Someone";
  lastAmount = OBSOverlay.formatAmount(e);
  hasData = true;
  render();
});

function applyFields(f) {
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  // Design px, not device px: template.css resolves it against the root scale.
  if (f.fontSize != null) set("--ov-size", String(Number(f.fontSize) || 24));
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  set("--ov-align", f.align === "center" ? "center" : f.align === "right" ? "right" : "left");

  mode = f.mode === "count" ? "count" : "latest";
  types = typesOf(String(f.eventType || "follower"));
  format = OBSOverlay.textField(f, "format", "Latest follower: {name}");
  emptyText = OBSOverlay.textField(f, "emptyText", "—");
  render();
}

function fill(tmpl) {
  return OBSOverlay.fillTemplate(tmpl || "", {
    name: lastName,
    count: OBSOverlay.formatCount(count),
    amount: lastAmount,
  });
}

function render() {
  // Latest mode needs an actor before it can render; count mode is valid from zero.
  // textContent keeps every substituted value escaped -- no markup injection.
  if (mode === "latest" && !hasData) {
    textEl.textContent = emptyText;
    return;
  }
  textEl.textContent = fill(format);
}
