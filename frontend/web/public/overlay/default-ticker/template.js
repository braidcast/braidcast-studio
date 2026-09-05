const ticker = document.getElementById("ticker");
const track = document.getElementById("track");
const idleEl = document.getElementById("idle");

// Per-platform tag label + color (mirrors the app's Events/Multichat palette). Also
// the actor-color fallback when an event carries no actorColor.
const PLATFORM = {
  twitch: { label: "Twitch", color: "#a970ff" },
  youtube: { label: "YouTube", color: "#ff4e45" },
  kick: { label: "Kick", color: "#53fc18" },
};

// Leading glyph per event type. Registry map, not a switch: a new type is one entry.
const TYPE_EMOJI = {
  follow: "🎉",
  sub: "⭐",
  resub: "🔄",
  subgift: "🎁",
  cheer: "💎",
  raid: "🚀",
  superchat: "💰",
  supersticker: "🎨",
  member: "🛡️",
};

// Format a money amount given in MINOR currency units (cents). Prefers the locale
// currency formatter; an unknown ISO code throws, so fall back to a bare form.
// Mirrors EventsDock.svelte so the ticker reads consistently with the app.
function money(amount, currency) {
  const value = amount / 100;
  if (currency) {
    try {
      return new Intl.NumberFormat(undefined, { style: "currency", currency }).format(value);
    } catch {
      // invalid currency code -- fall through to the plain form below.
    }
  }
  return (value.toFixed(2) + " " + (currency || "")).trim();
}

// One-line action phrasing per type, ported from EventsDock.svelte's SUMMARY map so
// the ticker matches the app. Unknown types fall back to the raw type string.
const SUMMARY = {
  follow: () => "followed",
  sub: (e) => "subscribed" + (e.tier ? " · " + e.tier : ""),
  resub: (e) => "resubscribed" + (e.months ? " · " + e.months + " months" : ""),
  subgift: (e) => {
    const n = e.count != null ? e.count : 1;
    return "gifted " + n + " sub" + (n === 1 ? "" : "s") + (e.tier ? " · " + e.tier : "");
  },
  cheer: (e) => "cheered " + (e.amount != null ? e.amount : 0) + " bits",
  raid: (e) => {
    const n = e.amount != null ? e.amount : 0;
    return "raided with " + n + " viewer" + (n === 1 ? "" : "s");
  },
  superchat: (e) => "Super Chat" + (e.amount != null ? " " + money(e.amount, e.currency) : ""),
  supersticker: (e) => "Super Sticker" + (e.amount != null ? " " + money(e.amount, e.currency) : ""),
  member: (e) =>
    e.months ? "member · " + e.months + " months" : e.tier ? "became a member · " + e.tier : "became a member",
};

// --- fields (live-applied) ---------------------------------------------------
let fontSize = 20;
let speed = 60; // design px/sec
// The belt is translated in real px, but the type it carries scales with the source, so
// the configured rate is resolved against the same root scale template.css sets -- left
// in device px a small source would run its text past several times too fast for its size.
// The design pixel is read from --ov-px rather than respelled here, so the belt's speed
// and the type it carries cannot come to disagree about what one is: that property is a
// rem coefficient, and the root font-size is one rem in device px, so their product is
// device px per design px. A fork that deletes the property leaves an unscaled belt
// rather than a NaN one, which would stop it dead.
let pxPerDesignPx = 1;
function readScale() {
  const root = getComputedStyle(document.documentElement);
  const remPerDesignPx = parseFloat(root.getPropertyValue("--ov-px"));
  pxPerDesignPx = Number.isFinite(remPerDesignPx) ? parseFloat(root.fontSize) * remPerDesignPx : 1;
}
readScale();
window.addEventListener("resize", readScale);
let separator = "  •  ";
let showPlatform = true;
let maxItems = 30;
let idleText = "";

// --- rolling event buffer + cyclic cursor ------------------------------------
// Recent events, capped at maxItems (oldest dropped). The conveyor cycles through
// this buffer endlessly, so a small feed still scrolls continuously; new events join
// the loop as the cursor comes around.
let buffer = [];
let cursor = 0;

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onEvent((e) => enqueue(e));

function applyFields(f) {
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  if (f.fontSize != null) {
    fontSize = Number(f.fontSize) || 20;
    // Design px, not device px: template.css resolves it against the root scale.
    set("--ov-size", String(fontSize));
  }
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));

  if (f.scrollSpeed != null) speed = Math.max(1, Number(f.scrollSpeed) || 60);
  if (f.separator != null) separator = String(f.separator);
  showPlatform = !!f.showPlatform;
  if (f.maxItems != null) {
    maxItems = Math.max(1, Number(f.maxItems) || 30);
    if (buffer.length > maxItems) buffer = buffer.slice(buffer.length - maxItems);
  }
  idleText = f.idleText != null ? String(f.idleText) : "";
  idleEl.textContent = idleText;

  ticker.classList.toggle("rtl", f.direction === "right");
}

function enqueue(e) {
  if (!e) return;
  const plat = PLATFORM[e.platform] || { label: String(e.platform || ""), color: "#888" };
  const sumFn = SUMMARY[e.type];
  buffer.push({
    platformLabel: plat.label,
    platformColor: plat.color,
    emoji: TYPE_EMOJI[e.type] || "",
    actorName: e.actorName || "Someone",
    actorColor: e.actorColor || plat.color,
    summary: sumFn ? sumFn(e) : String(e.type || ""),
    body: e.message || "",
  });
  if (buffer.length > maxItems) {
    buffer.shift();
    if (cursor > 0) cursor--;
  }
}

// Build one belt item. All user-supplied text goes through textContent (never
// innerHTML), so actor names / messages can't inject markup.
function makeTick(item) {
  const tick = document.createElement("span");
  tick.className = "tick";

  const inner = document.createElement("span");
  inner.className = "tick-inner";

  if (showPlatform && item.platformLabel) {
    const tag = document.createElement("span");
    tag.className = "tag";
    tag.style.background = item.platformColor;
    tag.textContent = item.platformLabel;
    inner.appendChild(tag);
  }

  if (item.emoji) {
    const em = document.createElement("span");
    em.className = "emoji";
    em.textContent = item.emoji;
    inner.appendChild(em);
  }

  const actor = document.createElement("span");
  actor.className = "actor";
  actor.style.color = item.actorColor;
  actor.textContent = item.actorName;
  inner.appendChild(actor);

  const sum = document.createElement("span");
  sum.className = "summary";
  sum.textContent = item.summary;
  inner.appendChild(sum);

  if (item.body) {
    const b = document.createElement("span");
    b.className = "body";
    b.textContent = "— " + item.body;
    inner.appendChild(b);
  }

  tick.appendChild(inner);

  const sep = document.createElement("span");
  sep.className = "sep";
  sep.textContent = separator;
  tick.appendChild(sep);

  return tick;
}

// --- conveyor ----------------------------------------------------------------
// Internal engine ALWAYS scrolls left (append at back, drop at front, both
// off-screen). Right-to-left is a pure CSS mirror on the flip layer, so this stays
// one code path and never rebuilds the belt -> no stutter when items append.
let tx = 0;
let last = 0;

function viewWidth() {
  return ticker.clientWidth || 0;
}

// Keep the belt filled to a viewport's worth of look-ahead beyond the right edge so
// there's never a gap. Spawns from the cyclic buffer; bails when the buffer is empty.
function topUp() {
  if (buffer.length === 0) return;
  // 4rem of slack past the look-ahead, resolved against the root scale like every other
  // length here: a fixed 64 device px is a different amount of belt on every source size.
  const target = viewWidth() * 2 + 64 * pxPerDesignPx;
  // Guard the loop: at most enough items to cover the target twice over.
  let guard = 4096;
  while (track.scrollWidth + tx < target && guard-- > 0) {
    track.appendChild(makeTick(buffer[cursor % buffer.length]));
    cursor++;
  }
}

function frame(now) {
  const dt = last ? Math.min(0.05, (now - last) / 1000) : 0;
  last = now;

  if (buffer.length === 0 && track.childElementCount === 0) {
    idleEl.classList.toggle("show", idleText.length > 0);
    requestAnimationFrame(frame);
    return;
  }
  idleEl.classList.remove("show");

  tx -= speed * pxPerDesignPx * dt;

  // Drop items that have fully scrolled off the left edge; add their width back to tx
  // so the remaining belt stays put as the DOM reflows.
  let first = track.firstElementChild;
  while (first && first.offsetWidth <= -tx) {
    tx += first.offsetWidth;
    track.removeChild(first);
    first = track.firstElementChild;
  }
  if (!track.firstElementChild) tx = 0;

  topUp();
  track.style.transform = "translateX(" + tx + "px)";
  requestAnimationFrame(frame);
}

requestAnimationFrame(frame);
