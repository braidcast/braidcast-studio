const titleEl = document.getElementById("goal-title");
const countEl = document.getElementById("goal-count");
const fillEl = document.getElementById("goal-fill");

// Each goal preset maps a stored goalType to what a matching event adds. Registry map, not
// a switch: a new goal is one entry. Every count goal is one of the Counter's sources
// (OBSOverlay.counter), so which events count and by how much is decided in one place:
//   followers   -> +1 per follow
//   subscribers -> +1 per sub or resub
//   gifted subs -> + the subs a gift gave (at least 1)
//   bits        -> + bits cheered
//   kicks       -> + Kicks sent (a count of Kick's gift currency, not money)
// Donations are money, which no Counter source sums: + e.amount, counting only events in
// the goal's currency. Amounts arrive in hundredths of the major unit, so a money goal is
// kept in hundredths too and its configured target/start are scaled to match. With no
// exchange rates to convert by, a donation in another currency is skipped rather than
// summed as though it were the same money.
const sourceGoal = (source) => ({ inc: (e) => OBSOverlay.counter.contribution(source, e) });
const GOALS = {
  followers: sourceGoal("follow"),
  subscribers: sourceGoal("sub"),
  giftedsubs: sourceGoal("subgift"),
  bits: sourceGoal("cheer"),
  kicks: sourceGoal("kicks"),
  donations: {
    inc: (e) => (["superchat", "supersticker"].indexOf(e.type) !== -1 && e.amount != null ? e.amount : 0),
    money: true,
  },
};

const normCurrency = (v) => String(v || "").trim().toUpperCase();

let goal = GOALS.followers;
let target = 50;
let seed = 0;
let title = "Goal";
let showPercent = true;
let currency = "USD";

// The progress is the seed plus this broadcast's matching events, so a source that reloads
// rebuilds it from the host's backfill instead of falling back to the seed. `events` holds the
// broadcast's real events by id: one can arrive both in the backfill and live, and is counted
// once. A test frame counts only in the editor preview, on top, until the next backfill.
const events = new Map();
let testExtra = 0;

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onBackfill((list) => {
  events.clear();
  for (const e of list) if (e && e.id) events.set(e.id, e);
  testExtra = 0;
  render();
});
OBSOverlay.onEvent((e) => {
  // A replay is a second showing of an event already counted.
  if (!e || e.replay) return;
  if (e.test) {
    if (OBSOverlay.preview && counts(e)) {
      testExtra += goal.inc(e);
      render();
    }
    return;
  }
  if (!e.id || events.has(e.id)) return;
  events.set(e.id, e);
  if (counts(e)) render();
});

function counts(e) {
  if (goal.inc(e) === 0) return false;
  return !goal.money || normCurrency(e.currency) === currency;
}

function current() {
  let n = seed + testExtra;
  for (const e of events.values()) if (counts(e)) n += goal.inc(e);
  return n;
}

function applyFields(f) {
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  // Design px, not device px: template.css resolves it against the root scale.
  if (f.fontSize != null) set("--ov-size", String(Number(f.fontSize) || 22));
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  if (f.barColor) set("--ov-bar", String(f.barColor));
  if (f.trackColor) set("--ov-track", String(f.trackColor));

  const goalType = String(f.goalType || "followers");
  goal = Object.prototype.hasOwnProperty.call(GOALS, goalType) ? GOALS[goalType] : GOALS.followers;
  currency = normCurrency(f.currency) || "USD";
  // Target and start are typed in whole units; a money goal counts in hundredths.
  const scale = goal.money ? 100 : 1;
  target = Math.round(Math.max(1, Number(f.target) || 50) * scale);
  // Seed from the configured offset so a streamer can start "already at 12".
  seed = Math.round((Number(f.startCurrent) || 0) * scale);
  title = f.title != null ? String(f.title) : "Goal";
  showPercent = f.showPercent !== false;
  render();
}

// Counts (followers/subs/bits/Kicks) render grouped; a money goal renders in its currency.
function fmt(n) {
  return goal.money ? OBSOverlay.formatMoney(n, currency) : OBSOverlay.formatCount(n);
}

function render() {
  const now = current();
  const pct = Math.max(0, Math.min(100, (now / target) * 100));
  fillEl.style.width = pct.toFixed(2) + "%";
  // All text via textContent -- user-supplied title can't inject markup.
  titleEl.textContent = title;
  let label = fmt(now) + " / " + fmt(target);
  if (showPercent) label += "  ·  " + Math.round(pct) + "%";
  countEl.textContent = label;
}
