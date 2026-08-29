const prefixEl = document.getElementById("prefix");
const valueEl = document.getElementById("value");

const pad = OBSOverlay.pad;

// Same shape as the uptime widget's FORMATS: `auto` drops the hours field until there is an
// hour to show, `hms` never does, `compact` steps down to seconds once the minutes run out.
// No toLocaleString -- a countdown reading is not a grouped quantity.
const FORMATS = {
  auto: (h, m, s) => (h > 0 ? h + ":" + pad(m) + ":" + pad(s) : m + ":" + pad(s)),
  hms: (h, m, s) => h + ":" + pad(m) + ":" + pad(s),
  compact: (h, m, s) => (h > 0 ? h + "h " + pad(m) + "m" : m > 0 ? m + "m " + pad(s) + "s" : s + "s"),
};

const ALIGN = { left: "left", center: "center", right: "right" };

let fields = {};
let timer = null;
// Captured once, at script start, so a non-looping cycle counts down from the moment the
// source loaded regardless of when the first render() runs. Any settings edit bumps the
// widget's rev, which changes the browser source's URL and forces OBS to reload it -- this
// script re-runs and loadedAt moves forward, so a non-looping countdown restarts from full
// length on every edit. Accepted, not a bug: persisting the start time would remove the
// only way to deliberately restart it; the real fix is the restart hotkey roadmap 9.3j
// defers.
const loadedAt = Date.now();

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));

function applyFields(f) {
  fields = f;
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  if (f.fontSize != null) set("--ov-size", (Number(f.fontSize) || 24) + "px");
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  set("--ov-align", ALIGN[String(f.align || "left")] || ALIGN.left);

  // Armed before the first render, not after: rendering first on a payoff would leave
  // nothing for render()'s own stop-guard below to clear, since the interval it checks for
  // would not exist yet -- it would then arm an interval that never gets stopped.
  if (timer === null) {
    timer = setInterval(render, 1000);
  }
  render();
}

// A cycle or dwell of 0, negative, or NaN would make the modulo/subtraction below divide by
// zero, spin, or leave the payoff branch permanently unreachable. Falls back to the schema
// default rather than propagating the bad value into render().
function positive(key, fallback) {
  const n = Number(fields[key]);
  return Number.isFinite(n) && n > 0 ? n : fallback;
}

function render() {
  const cycleMs = positive("cycleMinutes", 10) * 60000;
  const looping = OBSOverlay.isOn(fields, "loop", true);

  let remainingMs;
  let inPayoff;
  if (looping) {
    // The schema floors this at 1: a 0-second dwell makes phase < cycleMs always true, so
    // the payoff branch below would never run even though the field says it should.
    const dwellMs = positive("payoffSeconds", 10) * 1000;
    // The phase is read straight off the wall clock rather than tracked in a variable, so a
    // page reload mid-cycle lands on the same reading it would have shown without the
    // reload, and a missed tick never lets the reading drift.
    const phase = Date.now() % (cycleMs + dwellMs);
    inPayoff = phase >= cycleMs;
    remainingMs = cycleMs - phase;
  } else {
    const elapsed = Date.now() - loadedAt;
    inPayoff = elapsed >= cycleMs;
    remainingMs = cycleMs - elapsed;
  }

  if (!inPayoff) {
    // Ceil, not floor: at 900ms remaining the countdown must still read "1", not drop to
    // "0" a full second before the payoff phase actually begins.
    const totalSeconds = Math.ceil(remainingMs / 1000);
    const format = FORMATS[String(fields.format || "auto")] || FORMATS.auto;
    valueEl.classList.add("digits");
    // textContent throughout, so a user's prefix/payoff text cannot inject markup.
    valueEl.textContent = format(
      Math.floor(totalSeconds / 3600),
      Math.floor(totalSeconds / 60) % 60,
      totalSeconds % 60,
    );

    const prefix = OBSOverlay.textField(fields, "prefix", "");
    prefixEl.textContent = prefix;
    prefixEl.hidden = prefix.length === 0;
    return;
  }

  prefixEl.textContent = "";
  prefixEl.hidden = true;
  valueEl.classList.remove("digits");
  valueEl.textContent = OBSOverlay.textField(fields, "payoffText", "Let's go!");

  // A non-looping cycle holds the payoff forever once it is reached, so there is nothing
  // left to recompute -- the interval stops instead of ticking against a static reading.
  if (!looping && timer !== null) {
    clearInterval(timer);
    timer = null;
  }
}
