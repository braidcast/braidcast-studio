const deckEl = document.getElementById("deck");
const motionEl = document.getElementById("deck-motion");
const pileEl = document.getElementById("deck-idle");
const countEl = document.getElementById("deck-count");
const liveEl = document.getElementById("alert-live");
const cardTpl = document.getElementById("alert-card");

// Event type -> how the deck treats it (registry map, not a switch: a new type is one entry).
// What an alert of a type SAYS and how it LOOKS comes from its scope (OBSOverlay.resolveAlert:
// Defaults, then the event's override, then the winning variation); this table is only about
// how alerts share the deck.
//   group  the burst group it stacks with; without one, a type is a group of its own.
//   alone  plays alone, whole, with its own sound -- it never joins a burst and none joins
//          it. The value is the field key naming what its overflow card counts: that card
//          can follow another kind's burst, so it has to say what it stands for. That is
//          every tip (cheers, Super Chats, Super Stickers, Kicks), so whoever paid sees their
//          own name and message, and a raid.
const TYPES = {
  follow: {},
  sub: { group: "subs" },
  resub: { group: "subs" },
  subgift: { group: "subs" },
  member: { group: "members" },
  cheer: { alone: "nounCheer" },
  raid: { alone: "nounRaid" },
  superchat: { alone: "nounSuperchat" },
  supersticker: { alone: "nounSupersticker" },
  kicks: { alone: "nounKicks" },
};

// The card layouts the layout field offers; anything else reads as the default.
const LAYOUTS = { above: true, over: true, left: true, none: true };
// Media the card plays as video rather than as an image.
const VIDEO_FILE = /\.webm(?:[?#]|$)/i;

// Cards peeking out behind the front one.
const PEEK_DEPTH = 2;
// A burst gets `duration` for its last card plus this much, shared by every card before it.
const BURST_EXTRA_MS = 8000;
// The shortest a card may hold. A burst too big to fit at this pace ends on a "+N more" card.
const CARD_FLOOR_MS = 350;
// A "+N more" card is only worth it when it stands in for at least this many alerts.
const MIN_SUMMARIZED = 2;
// Bursts that may wait behind the one on the deck. Past this, an event of a burst group joins
// the newest waiting burst of its group whatever the burst window says, so a follow or bot
// storm stays one burst however long it runs; only a group with nothing waiting adds a burst.
// A type that plays alone (an `alone` TYPES entry: tips and raids) is capped separately, below.
// Nothing is dropped either way -- a big merged burst ends on its "+N more" card instead.
const MAX_WAITING = 3;
// Cards of one play-alone type that may wait, each whole, so whoever paid still gets their
// own card. Past this, more of that type fold into one waiting "+N more" card for the type,
// so a storm of one-bit cheers still ends in bounded time.
const MAX_ALONE_WAITING = 10;
// The least time the deck takes to leave, so a zero-length exit still hides before the next
// burst draws.
const MIN_LEAVE_MS = 60;
// Removes an exiting card this long after its exit should have finished, if the animation's
// own end never arrives. The queue itself never waits on either.
const EXIT_GRACE_MS = 150;
// A registry entry for `key`, or undefined. Own properties only: the keys come from event
// payloads and user settings, and "constructor" must not resolve to Object's.
const own = (map, key) => (Object.hasOwn(map, key) ? map[key] : undefined);
// The burst group `type` stacks with; `null` plays alone.
function groupOf(type) {
  const t = own(TYPES, type);
  return t?.alone ? null : t?.group || type;
}

const DEFAULTS = {
  duration: 5,
  burstWindow: 1.5,
  cardInterval: 1,
  volume: 80,
  msgBurstMore: "and {count} more!",
};

// The host injects the resolved fields before this script runs, so they are readable now
// rather than only from onLoad -- an event landing before the load frame still renders
// with the user's templates.
let fields = OBSOverlay.fields || {};
let queue = []; // bursts waiting for the deck, oldest first
let current = null; // the burst on the deck
let timer = 0; // the deck's one pending step; every step replaces it
let idleTimer = 0; // starts the idle loop once the entrance has played
let idleAnim = null;

OBSOverlay.onLoad((ctx) => {
  fields = ctx.fields || {};
  applyLook(fields);
});

// A burst's look: what its first alert resolves to. Every card of the burst shares it, so a
// burst reads as one alert; each card still says its own alert's message.
function lookOf(e) {
  return OBSOverlay.resolveAlert(e).settings;
}

OBSOverlay.onEvent((e) => {
  const now = performance.now();
  const group = groupOf(e.type);
  if (group !== null && seconds("burstWindow") > 0) {
    if (current && canJoin(current, group, now)) {
      current.events.push(e);
      current.lastAt = now;
      plan(current, now);
      return;
    }
    const tail = queue[queue.length - 1];
    if (tail && tail.group === group && now - tail.lastAt <= seconds("burstWindow") * 1000) {
      tail.events.push(e);
      tail.lastAt = now;
      return;
    }
  }
  if (group !== null && queue.length >= MAX_WAITING) {
    for (let i = queue.length - 1; i >= 0; i--) {
      if (queue[i].group === group) {
        queue[i].events.push(e);
        queue[i].lastAt = now;
        return;
      }
    }
  }
  const alone = group === null ? aloneBacklog(e.type) : null;
  if (alone?.overflow) {
    alone.overflow.events.push(e);
    alone.overflow.lastAt = now;
    alone.overflow.summaryFrom = 0;
    return;
  }
  const overflow = alone?.full === true;
  queue.push({
    group,
    overflow,
    look: lookOf(e),
    events: [e],
    lastAt: now,
    startAt: 0,
    front: 0,
    frontAt: 0,
    summaryFrom: -1,
    leaving: false,
  });
  if (!current) next();
});

// A moderator removed the words of events this deck holds: they leave every waiting alert,
// and the cards on the deck that showed them are drawn again without them. Such a card still
// sliding off goes at once rather than at the end of its exit; a deck already leaving is
// cleared, and so is what the live region last said.
OBSOverlay.onEventRedaction((ids, redacts) => {
  const hit = new Set(); // keys of the deck's cards that showed removed words
  for (const b of current ? [current, ...queue] : queue) {
    b.events.forEach((e, k) => {
      if (!e.message || !redacts(e)) return;
      delete e.message;
      if (b === current) hit.add("e" + k);
    });
  }
  if (hit.size === 0) return;
  if (current.leaving) {
    clearCards();
    liveEl.textContent = "";
    return;
  }
  for (const el of pileEl.querySelectorAll(".alert.exiting")) if (hit.has(el.dataset.key)) el.remove();
  for (const el of standing()) if (hit.has(el.dataset.key)) delete el.dataset.filled;
  layout(current);
  if (hit.has(cardKey(current, current.front))) announce(current);
});

// A play-alone type's waiting backlog: its overflow burst if one waits, and whether its whole
// cards have reached MAX_ALONE_WAITING -- then a new one starts the overflow burst, which
// shows as one "+N more" card once it holds two (a lone one still shows whole). It waits
// behind every whole card of its type, so those all play first.
function aloneBacklog(type) {
  let whole = 0;
  let overflow = null;
  for (const b of queue) {
    if (b.group !== null || b.events[0].type !== type) continue;
    if (b.overflow) overflow = b;
    else whole++;
  }
  return { overflow, full: whole >= MAX_ALONE_WAITING };
}

const actorLabel = (e) => e.actorName || "Someone";

// Template variable -> its value for an event. One table, so a new variable is one entry.
// A getter returning "" or nothing means the event carries no such value.
const VARS = {
  name: actorLabel,
  amount: (e) => OBSOverlay.formatAmount(e),
  amountText: (e) => OBSOverlay.formatAmountText(e),
  message: (e) => e.message,
  currency: (e) => e.currency,
  tier: (e) => e.tier,
  months: (e) => e.months,
  count: (e) => (e.count ? OBSOverlay.formatCount(e.count) : ""),
};

function valuesOf(e) {
  const values = {};
  for (const key in VARS) values[key] = VARS[key](e);
  return values;
}

// The card shows the name on its own line, so the message line drops the {name} it opens
// with, and the one space after it -- the whole of what render used to strip, fallback name
// included, so an unnamed actor never shows as "Someone Someone just followed!".
function withoutLeadingName(parts, shownName) {
  const i = parts.findIndex((p) => p.key === "name" && p.text === shownName);
  const after = parts[i + 1];
  if (i < 0 || !after || after.key !== null || !after.text.startsWith(" ")) return parts;
  const out = parts.slice();
  out.splice(i, 2, { text: after.text.slice(1), key: null });
  return out.filter((p) => p.text !== "");
}

// A numeric field in seconds. Zero is an answer (burstWindow 0 turns bursts off), so only
// a missing, negative or non-numeric value falls back.
function seconds(key, from = fields) {
  const v = Number(from[key]);
  return from[key] != null && from[key] !== "" && Number.isFinite(v) && v >= 0 ? v : DEFAULTS[key];
}

const durationMs = (b) => (Number(b.look.duration) || DEFAULTS.duration) * 1000;
const intervalMs = () => Math.max(CARD_FLOOR_MS, (seconds("cardInterval") || DEFAULTS.cardInterval) * 1000);

// A burst on the deck takes more of its group until it starts to leave, and until its time
// budget is spent -- unless a "+N more" card already stands at its end, which absorbs any
// number of latecomers without adding a moment to the burst. Nothing joins once another
// burst is waiting: alerts play in arrival order, so a latecomer never pushes back an alert
// that came before it. (Past MAX_WAITING the waiting queue gives that order up for the burst
// groups, to bound their wait; see onEvent.)
function canJoin(b, group, now) {
  return (
    queue.length === 0 &&
    b.group === group &&
    !b.leaving &&
    (b.summaryFrom >= 0 || now < b.startAt + BURST_EXTRA_MS)
  );
}

// Cards the burst shows: every event, or the ones before summaryFrom plus the "+N more" card.
const cardCount = (b) => (b.summaryFrom >= 0 ? b.summaryFrom + 1 : b.events.length);
const isSummary = (b, k) => b.summaryFrom >= 0 && k === b.summaryFrom;
const cardKey = (b, k) => (isSummary(b, k) ? "summary" : "e" + k);

// How long the front card holds, measured from when it came to the front. The last card
// holds the full duration; the ones before it split what is left of the burst's budget,
// never slower than cardInterval nor faster than the floor. When even the floor cannot fit
// them, the burst is cut to the cards that do fit and a "+N more" card.
function holdMs(b) {
  const last = cardCount(b) - 1;
  if (b.front >= last) return durationMs(b);
  const toGo = last - b.front;
  const left = Math.max(0, b.startAt + BURST_EXTRA_MS - b.frontAt);
  if (left / toGo >= CARD_FLOOR_MS) return Math.min(intervalMs(), left / toGo);
  if (b.summaryFrom < 0) {
    const fit = Math.max(1, Math.floor(left / CARD_FLOOR_MS));
    if (b.events.length - (b.front + fit) >= MIN_SUMMARIZED) b.summaryFrom = b.front + fit;
  }
  return CARD_FLOOR_MS;
}

// Re-derive the front card's hold and redraw the pile. Run on every change to the burst,
// so an event joining while the last card holds cuts that hold short to an interval.
function plan(b, now) {
  const hold = holdMs(b);
  layout(b);
  setStep(b.frontAt + hold - now, b.front >= cardCount(b) - 1 ? leave : advance);
}

function setStep(ms, fn) {
  clearTimeout(timer);
  timer = setTimeout(fn, Math.max(0, ms));
}

// The deck-wide styling a look carries. Defaults paint it at load; each burst repaints it.
function applyLook(look) {
  const style = deckEl.style;
  if (look.accent) style.setProperty("--accent", String(look.accent));
  if (look.textColor) style.setProperty("--alert-text", String(look.textColor));
  if (look.cardColor) style.setProperty("--alert-bg", String(look.cardColor));
  if (look.font) document.body.style.setProperty("--ov-font", String(look.font));
}

function stopIdle() {
  clearTimeout(idleTimer);
  if (idleAnim) idleAnim.cancel();
  idleAnim = null;
}

function next() {
  current = queue.shift() || null;
  if (!current) return;
  const now = performance.now();
  current.startAt = now;
  current.frontAt = now;
  clearCards();
  applyLook(current.look);
  plan(current, now);
  announce(current);
  deckEl.classList.add("show");
  const look = current.look;
  OBSOverlay.animate(motionEl, "in", look.inAnim);
  stopIdle();
  idleTimer = setTimeout(() => {
    idleAnim = OBSOverlay.animate(pileEl, "idle", look.idleAnim);
  }, OBSOverlay.animationMs("in", look.inAnim));
  if (look.sound) OBSOverlay.playSound(String(look.sound), seconds("volume", look) / 100);
}

function advance() {
  const b = current;
  exitFront(b);
  b.front++;
  b.frontAt = performance.now();
  plan(b, b.frontAt);
  announce(b);
}

function leave() {
  current.leaving = true;
  countEl.classList.remove("on");
  stopIdle();
  const out = current.look.outAnim;
  OBSOverlay.animate(motionEl, "out", out);
  setStep(Math.max(MIN_LEAVE_MS, OBSOverlay.animationMs("out", out)), () => {
    deckEl.classList.remove("show");
    for (const a of motionEl.getAnimations?.() ?? []) a.cancel();
    current = null;
    clearCards();
    next();
  });
}

function clearCards() {
  for (const el of pileEl.querySelectorAll(".alert")) el.remove();
}

const standing = () => pileEl.querySelectorAll(".alert:not(.exiting)");

function cardEl(key) {
  for (const el of standing()) if (el.dataset.key === key) return el;
  return null;
}

// The front card and the ones peeking behind it; anything else standing is dropped. A card
// new to the pile rises from behind it, except on a fresh deck, where the deck's own entry
// is the motion.
function layout(b) {
  // A "+N more" card decided while its slot already peeks takes that card over in place,
  // rather than the peek dropping out and a new card rising into the same spot.
  if (b.summaryFrom >= 0 && !cardEl("summary")) {
    const slot = cardEl("e" + b.summaryFrom);
    if (slot) slot.dataset.key = "summary";
  }
  const want = new Map();
  for (let d = 0; d <= PEEK_DEPTH && b.front + d < cardCount(b); d++) want.set(cardKey(b, b.front + d), d);
  for (const el of standing()) if (!want.has(el.dataset.key)) el.remove();
  const fresh = !pileEl.querySelector(".alert");
  for (const [key, d] of want) {
    let el = cardEl(key);
    if (!el) {
      el = cardTpl.content.firstElementChild.cloneNode(true);
      el.dataset.key = key;
      if (!fresh) {
        el.style.setProperty("--depth", String(PEEK_DEPTH + 1));
        el.classList.add("peek");
      }
      pileEl.insertBefore(el, countEl);
      if (!fresh) void el.offsetWidth; // commit the starting depth so the move animates
    }
    if (key === "summary") fillSummary(el, b);
    else if (!el.dataset.filled) fillCard(el, b.events[b.front + d], b.look);
    if (d === 0 && key !== "summary") playTextFx(el, b.look);
    el.style.setProperty("--depth", String(d));
    el.classList.toggle("peek", d > 0);
  }
  const multi = b.events.length > 1 && !isSummary(b, b.front);
  if (multi) countEl.textContent = b.front + 1 + " / " + b.events.length;
  countEl.classList.toggle("on", multi);
}

// The card's text, plain. Its text effect starts only when the card reaches the front (see
// playTextFx), so an effect that plays once is not spent while the card is still a peek.
function fillCard(el, e, look) {
  const shownName = actorLabel(e);
  const message = String(OBSOverlay.resolveAlert(e).settings.message || "{name}");
  const msg = withoutLeadingName(OBSOverlay.fillTemplateParts(message, valuesOf(e)), shownName);
  el.textParts = { name: [{ text: shownName, key: "name" }], msg };
  el.querySelector(".alert-name").textContent = shownName;
  el.querySelector(".alert-msg").textContent = msg.map((p) => p.text).join("");
  fillMedia(el, look);
  el.dataset.filled = "1";
  delete el.dataset.fx;
}

function playTextFx(el, look) {
  if (el.dataset.fx || !el.textParts) return;
  el.dataset.fx = "1";
  OBSOverlay.applyTextFx(el.querySelector(".alert-name"), el.textParts.name, look.textFx);
  OBSOverlay.applyTextFx(el.querySelector(".alert-msg"), el.textParts.msg, look.textFx);
}

// The card's image or video, placed by the look's layout. Anything that will not show --
// no media, a missing upload, a file that does not decode -- leaves a text-only card, and
// says so once in the log.
function fillMedia(el, look) {
  const box = el.querySelector(".alert-media");
  const url = String(look.media || "");
  const layout = own(LAYOUTS, look.layout) ? look.layout : "above";
  box.textContent = "";
  if (!url || layout === "none") {
    el.dataset.layout = "none";
    return;
  }
  const video = VIDEO_FILE.test(url);
  const media = document.createElement(video ? "video" : "img");
  const fail = () => {
    OBSOverlay.logOnce("media|" + url, "could not show alert media " + url.split("?")[0] + "; showing text only");
    media.remove();
    el.dataset.layout = "none";
  };
  media.addEventListener("error", fail, { once: true });
  if (video) {
    // Muted and looping: the alert's sound is the Sound field, and a clip shorter than the
    // alert repeats while a longer one is cut off when the card leaves.
    media.muted = true;
    media.loop = true;
    media.autoplay = true;
    media.playsInline = true;
  }
  media.src = url;
  box.appendChild(media);
  el.dataset.layout = layout;
}

function fillSummary(el, b) {
  const tmpl = OBSOverlay.textField(fields, "msgBurstMore", DEFAULTS.msgBurstMore);
  const more = OBSOverlay.formatCount(b.events.length - b.summaryFrom);
  // A burst's own "+N more" follows its cards and needs no label, so {type} is empty there.
  // An overflow card names its type: through {type} where the template places it, else on
  // the line below. A taken-over peek still holds its alert's line, so that is always
  // rewritten.
  const noun = b.overflow ? overflowNoun(b.events[0].type) : "";
  el.classList.add("summary");
  el.querySelector(".alert-name").textContent = OBSOverlay.fillTemplate(tmpl, { count: more, type: noun });
  el.querySelector(".alert-msg").textContent = tmpl.includes("{type}") ? "" : noun;
}

// What a play-alone type's overflow card counts, from the type's user-editable name field.
// A widget whose field is missing shows the type string.
function overflowNoun(type) {
  const key = own(TYPES, type)?.alone;
  return key ? OBSOverlay.textField(fields, key, type) : type;
}

// The live region says each card once, as it reaches the front. The cards themselves are
// aria-hidden, so the pile behind never gets read out.
function announce(b) {
  const el = cardEl(cardKey(b, b.front));
  if (!el) return;
  liveEl.textContent = [el.querySelector(".alert-name").textContent, el.querySelector(".alert-msg").textContent]
    .filter(Boolean)
    .join(" ");
}

// Sends the front card off with the burst exit. Frozen at its own size first, because once
// it leaves the flow the next card sizes the deck. Purely visual: the deck has already moved
// on, and the card is removed when its exit finishes or, failing that, on a timeout.
function exitFront(b) {
  const el = cardEl(cardKey(b, b.front));
  if (!el) return;
  const w = el.offsetWidth;
  el.style.width = w + "px";
  el.style.height = el.offsetHeight + "px";
  el.style.marginLeft = -w / 2 + "px";
  el.classList.add("exiting");
  const drop = () => el.remove();
  const exit = OBSOverlay.animate(el, "out", fields.burstExit);
  if (exit) exit.finished.then(drop, drop);
  setTimeout(drop, OBSOverlay.animationMs("out", fields.burstExit) + EXIT_GRACE_MS);
}
