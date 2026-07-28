const prefixEl = document.getElementById("prefix");
const valueEl = document.getElementById("value");

// One row per reading, taking whole hours/minutes/seconds. `auto` drops the hours field
// until there is an hour to show; `hms` never does, so a layout built around the widest
// reading keeps its width from the first second. Nothing here goes through
// toLocaleString: a clock reading is not a grouped quantity, and a thousands separator in
// the hours field -- or localized digits beside ASCII-padded minutes -- would read as
// damage rather than as formatting.
const FORMATS = {
  auto: (h, m, s) => (h > 0 ? h + ":" + pad(m) + ":" + pad(s) : m + ":" + pad(s)),
  hms: (h, m, s) => h + ":" + pad(m) + ":" + pad(s),
  compact: (h, m, s) => (h > 0 ? h + "h " + pad(m) + "m" : m > 0 ? m + "m " + pad(s) + "s" : s + "s"),
};

const ALIGN = { left: "left", center: "center", right: "right" };

let fields = {};
// The last broadcast state, or null before the first frame. The host replays `stream` on
// connect, so null spans only the moment before the SSE stream opens.
let state = null;
// Running only while there is an elapsed time to draw. A timer left going past the end of
// a broadcast is exactly the failure this widget must not have.
let timer = null;

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onStream((s) => {
  state = s;
  sync();
});

function applyFields(f) {
  fields = f;
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  if (f.fontSize != null) set("--ov-size", (Number(f.fontSize) || 24) + "px");
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  set("--ov-align", ALIGN[String(f.align || "left")] || ALIGN.left);

  sync();
}

// Checkbox reader with a per-field fallback for when the key is missing entirely (an
// older widget, or the user deleted the field).
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

function pad(n) {
  return n < 10 ? "0" + n : String(n);
}

// The one place the no-uptime states are decided, so render() holds no absence logic.
// null means there is nothing to count from, in any of three ways: not live; live but no
// output has signaled start yet (startedAt stays null through connecting, and a 0 epoch
// would read as an uptime of decades); or a start in the future, which only a clock
// adjustment produces and which must draw nothing rather than a negative reading.
function elapsedMs() {
  if (state === null || state.active !== true) {
    return null;
  }
  const started = state.startedAt;
  if (typeof started !== "number" || !Number.isFinite(started) || started <= 0) {
    return null;
  }
  const ms = Date.now() - started;
  return ms >= 0 ? ms : null;
}

// The tick recomputes from the wall clock rather than incrementing a counter: a counter
// drifts against real time, and it keeps climbing if a transition is ever missed. The
// interval only exists while there is something to count, so a stopped broadcast stops
// the timer with it.
function sync() {
  const running = elapsedMs() !== null;
  if (running && timer === null) {
    timer = setInterval(render, 1000);
  } else if (!running && timer !== null) {
    clearInterval(timer);
    timer = null;
  }
  render();
}

function render() {
  const ms = elapsedMs();
  if (ms === null) {
    // The prefix qualifies a duration, so it goes down with the one that is missing --
    // beside the placeholder it would qualify nothing.
    prefixEl.textContent = "";
    prefixEl.hidden = true;
    valueEl.textContent = isOn("showWhenOffline", false) ? textField("offlineText", "—") : "";
    return;
  }

  const total = Math.floor(ms / 1000);
  const format = FORMATS[String(fields.format || "auto")] || FORMATS.auto;
  // textContent throughout, so a user's prefix cannot inject markup.
  valueEl.textContent = format(Math.floor(total / 3600), Math.floor(total / 60) % 60, total % 60);

  const prefix = textField("prefix", "");
  prefixEl.textContent = prefix;
  prefixEl.hidden = prefix.length === 0;
}
