const deckEl = document.getElementById("deck");
const countEl = document.getElementById("deck-count");
const liveEl = document.getElementById("alert-live");
const cardTpl = document.getElementById("alert-card");

// Map event type -> the field key holding its message template. Unlisted types fall
// back to a generic line (registry map, not a switch: a new type is one entry).
const TEMPLATE_KEY = {
  follow: "msgFollow",
  sub: "msgSub",
  resub: "msgSub",
  subgift: "msgSub",
  cheer: "msgCheer",
  raid: "msgRaid",
  superchat: "msgSuperchat",
  supersticker: "msgSupersticker",
  member: "msgMember",
  kicks: "msgKicks",
};

// Event type -> the burst group it stacks with. An unlisted type is a group of its own.
// `null` always plays alone, whole, with its own sound -- it never joins a burst and none
// joins it. That is every tip (cheers, Super Chats, Super Stickers, Kicks), so whoever paid
// sees their own name and message, and a raid.
const BURST_GROUP = {
  sub: "subs",
  resub: "subs",
  subgift: "subs",
  member: "members",
  cheer: null,
  raid: null,
  superchat: null,
  supersticker: null,
  kicks: null,
};

// burstAnimation value -> the class that plays a card's exit (one keyframes block each in
// template.css). An unknown value plays the default's.
const EXIT_CLASS = {
  slide: "exit-slide",
  flip: "exit-flip",
  fall: "exit-fall",
  fade: "exit-fade",
  zoom: "exit-zoom",
};

// Cards peeking out behind the front one.
const PEEK_DEPTH = 2;
// A burst gets `duration` for its last card plus this much, shared by every card before it.
const BURST_EXTRA_MS = 8000;
// The shortest a card may hold. A burst too big to fit at this pace ends on a "+N more" card.
const CARD_FLOOR_MS = 350;
// A "+N more" card is only worth it when it stands in for at least this many alerts.
const MIN_SUMMARIZED = 2;
// Outlasts the deck's 320 ms hide transition in template.css.
const HIDE_MS = 360;
// Removes an exiting card if its animationend never arrives. Longer than every exit
// animation in template.css; the queue itself never waits on either.
const EXIT_REMOVE_MS = 700;
// A registry entry for `key`, or undefined. Own properties only: the keys come from event
// payloads and user settings, and "constructor" must not resolve to Object's.
const own = (map, key) => (Object.hasOwn(map, key) ? map[key] : undefined);

const DEFAULTS = {
  duration: 5,
  burstWindow: 1.5,
  cardInterval: 1,
  burstAnimation: "slide",
  msgBurstMore: "and {count} more!",
};

// The host injects the resolved fields before this script runs, so they are readable now
// rather than only from onLoad -- an event landing before the load frame still renders
// with the user's templates.
let fields = OBSOverlay.fields || {};
let queue = []; // bursts waiting for the deck, oldest first
let current = null; // the burst on the deck
let timer = 0; // the deck's one pending step; every step replaces it

OBSOverlay.onLoad((ctx) => {
  fields = ctx.fields || {};
  if (fields.accent) document.documentElement.style.setProperty("--accent", String(fields.accent));
  if (fields.font) document.body.style.setProperty("--ov-font", String(fields.font));
});

OBSOverlay.onEvent((e) => {
  const now = performance.now();
  const group = Object.hasOwn(BURST_GROUP, e.type) ? BURST_GROUP[e.type] : e.type;
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
  queue.push({ group, events: [e], lastAt: now, startAt: 0, front: 0, frontAt: 0, summaryFrom: -1, leaving: false });
  if (!current) next();
});

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

function render(tmpl, e) {
  const values = {};
  for (const key in VARS) values[key] = VARS[key](e);
  return OBSOverlay.fillTemplate(tmpl || "", values);
}

// A numeric field in seconds. Zero is an answer (burstWindow 0 turns bursts off), so only
// a missing, negative or non-numeric value falls back.
function seconds(key) {
  const v = Number(fields[key]);
  return fields[key] != null && fields[key] !== "" && Number.isFinite(v) && v >= 0 ? v : DEFAULTS[key];
}

const durationMs = () => (Number(fields.duration) || DEFAULTS.duration) * 1000;
const intervalMs = () => Math.max(CARD_FLOOR_MS, (seconds("cardInterval") || DEFAULTS.cardInterval) * 1000);

// A burst on the deck takes more of its group until it starts to leave, and until its time
// budget is spent -- unless a "+N more" card already stands at its end, which absorbs any
// number of latecomers without adding a moment to the burst. Nothing joins once another
// burst is waiting: alerts play in arrival order, so a latecomer never pushes back an alert
// that came before it.
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
  if (b.front >= last) return durationMs();
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

function next() {
  current = queue.shift() || null;
  if (!current) return;
  const now = performance.now();
  current.startAt = now;
  current.frontAt = now;
  clearCards();
  plan(current, now);
  announce(current);
  deckEl.classList.add("show");
  if (fields.sound) OBSOverlay.playSound(String(fields.sound), 1);
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
  deckEl.classList.remove("show");
  setStep(HIDE_MS, () => {
    current = null;
    clearCards();
    next();
  });
}

function clearCards() {
  for (const el of deckEl.querySelectorAll(".alert")) el.remove();
}

const standing = () => deckEl.querySelectorAll(".alert:not(.exiting)");

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
  const fresh = !deckEl.querySelector(".alert");
  for (const [key, d] of want) {
    let el = cardEl(key);
    if (!el) {
      el = cardTpl.content.firstElementChild.cloneNode(true);
      el.dataset.key = key;
      if (!fresh) {
        el.style.setProperty("--depth", String(PEEK_DEPTH + 1));
        el.classList.add("peek");
      }
      deckEl.insertBefore(el, countEl);
      if (!fresh) void el.offsetWidth; // commit the starting depth so the move animates
    }
    if (key === "summary") fillSummary(el, b);
    else if (!el.dataset.filled) fillCard(el, b.events[b.front + d]);
    el.style.setProperty("--depth", String(d));
    el.classList.toggle("peek", d > 0);
  }
  const multi = b.events.length > 1 && !isSummary(b, b.front);
  if (multi) countEl.textContent = b.front + 1 + " / " + b.events.length;
  countEl.classList.toggle("on", multi);
}

function fillCard(el, e) {
  const key = own(TEMPLATE_KEY, e.type);
  const tmpl = (key && fields[key]) || "{name}";
  // The strip has to remove exactly what render() substituted for {name}, fallback
  // included -- otherwise an unnamed actor shows as "Someone Someone just followed!".
  const shownName = actorLabel(e);
  el.querySelector(".alert-name").textContent = shownName;
  el.querySelector(".alert-msg").textContent = render(tmpl, e).replace(shownName + " ", "");
  el.dataset.filled = "1";
}

function fillSummary(el, b) {
  const tmpl = OBSOverlay.textField(fields, "msgBurstMore", DEFAULTS.msgBurstMore);
  const more = OBSOverlay.formatCount(b.events.length - b.summaryFrom);
  el.classList.add("summary");
  el.querySelector(".alert-name").textContent = OBSOverlay.fillTemplate(tmpl, { count: more });
  el.querySelector(".alert-msg").textContent = ""; // a taken-over peek still holds its alert's line
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

// Sends the front card off with the chosen exit. Frozen at its own size first, because once
// it leaves the flow the next card sizes the deck. Purely visual: the deck has already moved
// on, and the card is removed on animationend or, failing that, on a timeout.
function exitFront(b) {
  const el = cardEl(cardKey(b, b.front));
  if (!el) return;
  const w = el.offsetWidth;
  el.style.width = w + "px";
  el.style.height = el.offsetHeight + "px";
  el.style.marginLeft = -w / 2 + "px";
  el.classList.add("exiting", own(EXIT_CLASS, fields.burstAnimation) || EXIT_CLASS[DEFAULTS.burstAnimation]);
  const drop = () => el.remove();
  el.addEventListener("animationend", drop, { once: true });
  setTimeout(drop, EXIT_REMOVE_MS);
}
