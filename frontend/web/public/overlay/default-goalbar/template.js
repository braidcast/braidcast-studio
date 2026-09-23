const titleEl = document.getElementById("goal-title");
const countEl = document.getElementById("goal-count");
const fillEl = document.getElementById("goal-fill");

// Each goal preset maps a user-facing goal to the real NormalizedEvent `type`
// strings (verbatim from EventsDock/alert-box) and how much a matching event adds.
// Registry map, not a switch: a new goal is one entry.
//   followers   -> +1 per follow
//   subscribers -> +1 per sub or resub
//   gifted subs -> + e.count (a community gift of N subs counts as N)
//   bits        -> + e.amount (bits cheered)
//   donations   -> + e.amount, counting only events in the goal's currency. Amounts
//                  arrive in hundredths of the major unit, so a money goal is kept in
//                  hundredths too and its configured target/start are scaled to match.
//                  With no exchange rates to convert by, a donation in another currency
//                  is skipped rather than summed as though it were the same money.
const GOALS = {
  followers: { types: ["follow"], inc: () => 1 },
  subscribers: { types: ["sub", "resub"], inc: () => 1 },
  giftedsubs: { types: ["subgift"], inc: (e) => (e.count != null ? e.count : 1) },
  bits: { types: ["cheer"], inc: (e) => (e.amount != null ? e.amount : 0) },
  donations: { types: ["superchat", "supersticker"], inc: (e) => (e.amount != null ? e.amount : 0), money: true },
};

const normCurrency = (v) => String(v || "").trim().toUpperCase();

let goal = GOALS.followers;
let target = 50;
let current = 0;
let title = "Goal";
let showPercent = true;
let currency = "USD";

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onEvent((e) => {
  if (!e || goal.types.indexOf(e.type) === -1) return;
  if (goal.money && normCurrency(e.currency) !== currency) return;
  current += goal.inc(e);
  render();
});

function applyFields(f) {
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  // Design px, not device px: template.css resolves it against the root scale.
  if (f.fontSize != null) set("--ov-size", String(Number(f.fontSize) || 22));
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  if (f.barColor) set("--ov-bar", String(f.barColor));
  if (f.trackColor) set("--ov-track", String(f.trackColor));

  goal = GOALS[String(f.goalType || "followers")] || GOALS.followers;
  currency = normCurrency(f.currency) || "USD";
  // Target and start are typed in whole units; a money goal counts in hundredths.
  const scale = goal.money ? 100 : 1;
  target = Math.round(Math.max(1, Number(f.target) || 50) * scale);
  // Seed from the configured offset so a streamer can start "already at 12".
  current = Math.round((Number(f.startCurrent) || 0) * scale);
  title = f.title != null ? String(f.title) : "Goal";
  showPercent = f.showPercent !== false;
  render();
}

// Counts (followers/subs/bits) render grouped; a money goal renders in its currency.
function fmt(n) {
  return goal.money ? OBSOverlay.formatMoney(n, currency) : OBSOverlay.formatCount(n);
}

function render() {
  const pct = Math.max(0, Math.min(100, (current / target) * 100));
  fillEl.style.width = pct.toFixed(2) + "%";
  // All text via textContent -- user-supplied title can't inject markup.
  titleEl.textContent = title;
  let label = fmt(current) + " / " + fmt(target);
  if (showPercent) label += "  ·  " + Math.round(pct) + "%";
  countEl.textContent = label;
}
