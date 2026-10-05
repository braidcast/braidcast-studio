const wallEl = document.getElementById("wall");

// Which platforms' chat feeds the wall, by field.
const PLATFORM_FIELDS = { twitch: "showTwitch", youtube: "showYouTube", kick: "showKick" };

// Each motion's path across the frame, as keyframes over the emote's lifetime. `x` and `y`
// are where it starts, in device px, and `w`, `h` the frame's size. Data, not branches: a new
// motion is one row here and an option in fields.json.
const MOTIONS = {
  float: {
    start: (w, h) => ({ x: Math.random() * w, y: h }),
    frames: (p, w, h, size) => [
      { transform: `translate(${p.x}px, ${h}px)`, opacity: 0 },
      { transform: `translate(${p.x + size * 0.4}px, ${h * 0.7}px)`, opacity: 1, offset: 0.15 },
      { transform: `translate(${p.x - size * 0.4}px, ${h * 0.35}px)`, opacity: 1, offset: 0.6 },
      { transform: `translate(${p.x}px, ${-size}px)`, opacity: 0 },
    ],
    easing: "linear",
  },
  rain: {
    start: (w) => ({ x: Math.random() * w, y: 0 }),
    frames: (p, w, h, size) => [
      { transform: `translate(${p.x}px, ${-size}px) rotate(-12deg)`, opacity: 1 },
      { transform: `translate(${p.x}px, ${h}px) rotate(12deg)`, opacity: 1 },
    ],
    easing: "cubic-bezier(0.55, 0, 1, 0.45)",
  },
  pop: {
    start: (w, h) => ({ x: Math.random() * w, y: Math.random() * h }),
    frames: (p) => [
      { transform: `translate(${p.x}px, ${p.y}px) scale(0)`, opacity: 0 },
      { transform: `translate(${p.x}px, ${p.y}px) scale(1.15)`, opacity: 1, offset: 0.12 },
      { transform: `translate(${p.x}px, ${p.y}px) scale(1)`, opacity: 1, offset: 0.2 },
      { transform: `translate(${p.x}px, ${p.y}px) scale(1)`, opacity: 1, offset: 0.85 },
      { transform: `translate(${p.x}px, ${p.y}px) scale(0.6)`, opacity: 0 },
    ],
    easing: "ease-out",
  },
};

const reducedMotion = !!(window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches);
const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

let fields = {};
// The emotes on screen, oldest first, each with the chat line it came from, so a moderator's
// removal takes a removed message's emotes off the wall too.
const live = [];

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onChat((m) => spawnFrom(m));
OBSOverlay.onChatModeration((op, removes) => {
  for (const item of live.slice()) {
    if (item.identity && removes(item.identity)) drop(item);
  }
});

function isOn(key, fallback) {
  const v = fields[key];
  if (v == null) return fallback;
  return v === true || v === "true";
}

function num(key, fallback, min, max) {
  const n = Number(fields[key]);
  return Number.isFinite(n) ? Math.max(min, Math.min(max, n)) : fallback;
}

function applyFields(f) {
  fields = f;
  document.documentElement.style.setProperty("--ov-emote", String(num("emoteSize", 72, 24, 192)));
}

// The emotes a chat line sends, capped per message. Only https images: an emote fragment's
// url comes from a platform or an emote provider, and anything else is not an emote.
function emotesOf(m) {
  const out = [];
  for (const f of (m && m.fragments) || []) {
    if (f && f.type === "emote" && typeof f.url === "string" && f.url.indexOf("https://") === 0) out.push(f);
  }
  return out.slice(0, Math.round(num("perMessage", 5, 1, 20)));
}

// A test line plays on stream too, like a test alert: it is a moment, gone in seconds, and
// how a streamer checks the wall in the real scene.
function spawnFrom(m) {
  if (!m) return;
  if (own(PLATFORM_FIELDS, m.platform) && !isOn(PLATFORM_FIELDS[m.platform], true)) return;
  const identity = OBSOverlay.chatIdentity(m);
  for (const f of emotesOf(m)) spawn(f, identity);
}

function spawn(fragment, identity) {
  const max = Math.round(num("maxOnScreen", 60, 5, 150));
  while (live.length >= max) drop(live[0]);

  const img = document.createElement("img");
  img.className = "emote";
  img.alt = "";
  img.src = fragment.url;
  wallEl.appendChild(img);
  const item = { img, identity, timer: 0, anim: null };
  live.push(item);

  const ms = num("lifetime", 6, 2, 15) * 1000;
  const w = wallEl.clientWidth;
  const h = wallEl.clientHeight;
  const size = img.offsetWidth || 72;
  const motion = own(MOTIONS, fields.motion) ? MOTIONS[fields.motion] : MOTIONS.float;
  const at = motion.start(Math.max(0, w - size), Math.max(0, h - size));
  if (typeof img.animate === "function") {
    const frames = reducedMotion
      ? fadeInPlace(at.x, Math.random() * Math.max(0, h - size))
      : motion.frames(at, w, h, size);
    item.anim = img.animate(frames, {
      duration: ms,
      easing: reducedMotion ? "linear" : motion.easing,
      fill: "both",
    });
  }
  item.timer = setTimeout(() => drop(item), ms + 50);
}

// Reduced motion: the emote appears where it lands, holds, and fades.
function fadeInPlace(x, y) {
  const transform = `translate(${x}px, ${y}px)`;
  return [
    { transform, opacity: 0 },
    { transform, opacity: 1, offset: 0.15 },
    { transform, opacity: 1, offset: 0.85 },
    { transform, opacity: 0 },
  ];
}

function drop(item) {
  const i = live.indexOf(item);
  if (i === -1) return;
  live.splice(i, 1);
  clearTimeout(item.timer);
  if (item.anim) item.anim.cancel();
  item.img.remove();
}
