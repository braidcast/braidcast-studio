const textEl = document.getElementById("label-text");

// Map the user-facing event choice to the real NormalizedEvent `type` strings
// (verbatim from EventsDock/alert-box). Registry map, not a switch. "subscriber"
// counts resubs too so a running total reflects every sub event.
const EVENTS = {
  follower: ["follow"],
  subscriber: ["sub", "resub"],
  giftedsub: ["subgift"],
  cheer: ["cheer"],
  raid: ["raid"],
  superchat: ["superchat"],
};

let types = EVENTS.follower;
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
  lastAmount = e.amount != null ? String(e.amount) : "";
  hasData = true;
  render();
});

function applyFields(f) {
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  if (f.fontSize != null) set("--ov-size", (Number(f.fontSize) || 24) + "px");
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  set("--ov-align", f.align === "center" ? "center" : f.align === "right" ? "right" : "left");

  mode = f.mode === "count" ? "count" : "latest";
  types = EVENTS[String(f.eventType || "follower")] || EVENTS.follower;
  format = f.format != null ? String(f.format) : "Latest follower: {name}";
  emptyText = f.emptyText != null ? String(f.emptyText) : "—";
  render();
}

function fill(tmpl) {
  return String(tmpl || "")
    .replaceAll("{name}", lastName || "")
    .replaceAll("{count}", String(count))
    .replaceAll("{amount}", lastAmount || "");
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
