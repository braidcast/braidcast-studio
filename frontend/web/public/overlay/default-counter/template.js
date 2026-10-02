const counterEl = document.getElementById("counter");
const textEl = document.getElementById("counter-text");
// The counting rules live in the runtime, so this page only decides what to draw.
const C = OBSOverlay.counter;

// Platform id + the field that ticks it. One row per platform: adding one is a row here and
// a checkbox in fields.json. Order mirrors the app's PLATFORM_ORDER.
const PLATFORMS = [
  { id: "twitch", showKey: "showTwitch" },
  { id: "youtube", showKey: "showYouTube" },
  { id: "kick", showKey: "showKick" },
  { id: "facebook", showKey: "showFacebook" },
];

const ALIGN = { left: "left", center: "center", right: "right" };
const DIGIT = /[0-9]/;
const reducedMotion = window.matchMedia ? window.matchMedia("(prefers-reduced-motion: reduce)") : null;

// The resolved fields are on the page before onLoad fires, and a count can land before it
// does, so a render never has to run against an empty field set.
let fields = OBSOverlay.fields || {};
// The runtime's session tally, once it hands one over. Null until then.
let tally = null;
// The last reading of each total, or null when none has arrived. Null is "nothing has
// reported", never zero.
let viewers = null;
let channels = null;
// The figure on screen: a number, null while the idle text is up, or undefined when the
// markup has nothing in it yet -- the one state a change must not animate out of.
let shown = undefined;
// The number elements, one per {n} in the format, and what they were built for.
let nums = [];
let layoutKey = null;
// The running count-up, or null.
let tween = null;

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
// Only a real event arriving animates. A seed is the page catching up to the count it already
// had (a reload mid-broadcast must not roll up from 0), and a window change is a new
// broadcast starting over.
OBSOverlay.onSessionTally((t, cause) => {
  tally = t;
  if (isSessionSource()) {
    update(cause === "event");
  }
});
OBSOverlay.onViewers((v) => {
  viewers = v;
  if (source() === "viewers") {
    update(true);
  }
});
OBSOverlay.onChannelStats((s) => {
  channels = s;
  if (source() === "followers") {
    update(true);
  }
});
// The viewer poller stops with the broadcast without a closing zero, so an ended broadcast
// clears the reading back to "nothing reported" -- the idle text, as the viewer count widget
// does -- rather than leaving the last audience up.
OBSOverlay.onStream((s) => {
  if (s && s.active !== true && viewers !== null) {
    viewers = null;
    if (source() === "viewers") {
      update(false);
    }
  }
});

function applyFields(f) {
  fields = f;
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  // Design px, not device px: template.css resolves it against the root scale.
  if (f.fontSize != null) set("--ov-size", String(Number(f.fontSize) || 24));
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  set("--ov-align", ALIGN[String(f.align || "left")] || ALIGN.left);
  update(false);
}

function source() {
  return String(fields.source || "follow");
}

function isSessionSource() {
  return Object.prototype.hasOwnProperty.call(C.sources, source());
}

function platforms() {
  return new Set(PLATFORMS.filter((p) => OBSOverlay.isOn(fields, p.showKey, true)).map((p) => p.id));
}

// What to show: a number, null for the idle text, or undefined while a session count is
// still waiting on the host's tally -- drawing a since-load 0 in that gap is the flash a
// reload mid-broadcast would otherwise show.
function value() {
  const src = source();
  if (isSessionSource()) {
    if (!tally || !tally.ready) {
      return undefined;
    }
    return C.withOffset(tally.count(src, platforms()), fields.offset);
  }
  if (src === "viewers") {
    return C.withOffset(viewers ? C.viewerTotal(viewers.perPlatform, platforms()) : null, fields.offset);
  }
  if (src === "followers") {
    return C.withOffset(channels ? C.audienceTotal(channels.perPlatform, platforms()) : null, fields.offset);
  }
  // A source this build does not know (a newer or hand-edited document) has no figure.
  return null;
}

function rollMs() {
  const v = parseFloat(getComputedStyle(document.documentElement).getPropertyValue("--ov-roll-ms"));
  return Number.isFinite(v) && v > 0 ? v : 600;
}

function update(animate) {
  const next = value();
  if (next === undefined) {
    counterEl.hidden = true;
    return;
  }
  const mode = C.effectiveAnimation(fields.animation, !!(reducedMotion && reducedMotion.matches));
  ensureLayout(mode);
  counterEl.hidden = false;
  const prev = shown;
  shown = next;
  if (!animate || mode === "none" || next === null || prev == null || prev === next) {
    draw(next, true);
    return;
  }
  if (mode === "countup") {
    countUp(prev, next);
  } else if (mode === "pop") {
    draw(next, true);
    pop();
  } else {
    draw(next, false);
  }
}

// Build the format's markup once per format and animation: text around one .num per {n}.
// Everything lands through textContent, so a format cannot inject markup.
function ensureLayout(mode) {
  const format = OBSOverlay.textField(fields, "format", "{n}");
  const key = mode + "\n" + format;
  if (key === layoutKey) {
    return;
  }
  layoutKey = key;
  stopTween();
  textEl.textContent = "";
  nums = [];
  const parts = C.templateParts(format);
  parts.forEach((part, i) => {
    if (part) {
      textEl.appendChild(document.createTextNode(part));
    }
    if (i < parts.length - 1) {
      const num = document.createElement("span");
      num.className = "num";
      textEl.appendChild(num);
      nums.push(num);
    }
  });
  shown = undefined;
}

function draw(n, instant) {
  stopTween();
  const odometer = layoutKey !== null && layoutKey.startsWith("odometer\n");
  for (const el of nums) {
    if (n === null) {
      el.textContent = OBSOverlay.textField(fields, "idleText", "—");
    } else if (odometer) {
      setOdometer(el, OBSOverlay.formatCount(n), instant);
    } else {
      el.textContent = OBSOverlay.formatCount(n);
    }
  }
}

function writeAll(n) {
  const text = OBSOverlay.formatCount(n);
  for (const el of nums) {
    el.textContent = text;
  }
}

function stopTween() {
  if (tween) {
    cancelAnimationFrame(tween.raf);
    tween = null;
  }
}

// A change mid-count carries on from the figure currently on screen, not from where the
// interrupted count started.
function countUp(prev, next) {
  const from = tween ? tween.current : prev;
  stopTween();
  const t0 = performance.now();
  const ms = rollMs();
  tween = { current: from, raf: 0 };
  const step = (now) => {
    const t = (now - t0) / ms;
    const v = C.tweenValue(from, next, t);
    tween.current = v;
    writeAll(v);
    if (t < 1) {
      tween.raf = requestAnimationFrame(step);
    } else {
      tween = null;
    }
  };
  tween.raf = requestAnimationFrame(step);
}

function pop() {
  for (const el of nums) {
    if (typeof el.animate === "function") {
      el.animate([{ transform: "scale(1)" }, { transform: "scale(1.18)", offset: 0.35 }, { transform: "scale(1)" }], {
        duration: 320,
        easing: "ease-out",
      });
    }
  }
}

function digitColumn() {
  const col = document.createElement("span");
  col.className = "odo-col";
  const strip = document.createElement("span");
  strip.className = "odo-strip";
  strip.setAttribute("aria-hidden", "true");
  for (let d = 0; d <= 9; d++) {
    const cell = document.createElement("span");
    cell.textContent = String(d);
    strip.appendChild(cell);
  }
  col.appendChild(strip);
  return col;
}

// Rolling digits. Cells are matched from the right, so the ones column stays the ones column
// as the number grows and only a new leading digit is created -- which then rolls up from 0.
// Separators are static text cells between the columns.
function setOdometer(el, text, instant) {
  const old = Array.from(el.children);
  const chars = Array.from(text);
  const cells = chars.map((ch, i) => {
    const prev = old[old.length - chars.length + i];
    const digit = DIGIT.test(ch);
    if (prev && prev.classList.contains("odo-col") === digit) {
      return prev;
    }
    if (digit) {
      return digitColumn();
    }
    const sep = document.createElement("span");
    sep.className = "odo-sep";
    return sep;
  });
  el.replaceChildren(...cells);
  el.classList.toggle("instant", instant);
  // Commit new columns at 0 before moving them, so they roll rather than appear in place.
  void el.offsetWidth;
  cells.forEach((cell, i) => {
    if (cell.classList.contains("odo-col")) {
      cell.firstChild.style.transform = "translateY(" + -Number(chars[i]) * 10 + "%)";
    } else {
      cell.textContent = chars[i];
    }
  });
  if (instant) {
    void el.offsetWidth;
    el.classList.remove("instant");
  }
}
