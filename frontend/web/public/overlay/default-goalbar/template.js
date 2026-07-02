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
//   donations   -> + e.amount/100 (super chat/sticker amounts arrive in MINOR
//                  currency units / cents, so divide to reach whole currency)
const GOALS = {
  followers: { types: ["follow"], inc: () => 1 },
  subscribers: { types: ["sub", "resub"], inc: () => 1 },
  giftedsubs: { types: ["subgift"], inc: (e) => (e.count != null ? e.count : 1) },
  bits: { types: ["cheer"], inc: (e) => (e.amount != null ? e.amount : 0) },
  donations: { types: ["superchat", "supersticker"], inc: (e) => (e.amount != null ? e.amount / 100 : 0) },
};

let goal = GOALS.followers;
let target = 50;
let current = 0;
let title = "Goal";
let showPercent = true;

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onEvent((e) => {
  if (!e || goal.types.indexOf(e.type) === -1) return;
  current += goal.inc(e);
  render();
});

function applyFields(f) {
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  if (f.fontSize != null) set("--ov-size", (Number(f.fontSize) || 22) + "px");
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  if (f.barColor) set("--ov-bar", String(f.barColor));
  if (f.trackColor) set("--ov-track", String(f.trackColor));

  goal = GOALS[String(f.goalType || "followers")] || GOALS.followers;
  target = Math.max(1, Number(f.target) || 50);
  // Seed from the configured offset so a streamer can start "already at 12".
  current = Number(f.startCurrent) || 0;
  title = f.title != null ? String(f.title) : "Goal";
  showPercent = f.showPercent !== false;
  render();
}

// Integer counts (followers/subs/bits) render bare; donations may be fractional.
function fmt(n) {
  return Number.isInteger(n) ? String(n) : n.toFixed(2);
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
