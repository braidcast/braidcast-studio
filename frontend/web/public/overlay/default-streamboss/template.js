const bossEl = document.getElementById("boss");
const nameEl = document.getElementById("name");
const fillEl = document.getElementById("fill");
const hpEl = document.getElementById("hp");
const hitEl = document.getElementById("hit");

// What one event does to the boss: the damage field (with fields.json's default, for a page
// whose fields leave it out), and how many times it applies. Registry map, not a switch: a
// new event type is one row here and a field in fields.json.
const DAMAGE = {
  follow: { field: "dmgFollow", fallback: 10, times: () => 1 },
  sub: { field: "dmgSub", fallback: 100, times: () => 1 },
  resub: { field: "dmgSub", fallback: 100, times: () => 1 },
  subgift: { field: "dmgSub", fallback: 100, times: (e) => (e.count > 0 ? e.count : 1) },
  member: { field: "dmgMember", fallback: 100, times: () => 1 },
  cheer: { field: "dmgBit", fallback: 1, times: (e) => (e.amount > 0 ? e.amount : 0) },
  superchat: { field: "dmgSuperChat", fallback: 100, times: () => 1 },
  supersticker: { field: "dmgSuperChat", fallback: 100, times: () => 1 },
  kicks: { field: "dmgKick", fallback: 1, times: (e) => (e.amount > 0 ? e.amount : 0) },
  raid: { field: "dmgRaider", fallback: 1, times: (e) => (e.amount > 0 ? e.amount : 0) },
};

const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

let fields = {};
// This broadcast's events as this page knows them, oldest first, by id: the backfill on
// connect, then live ones, each once. The boss is a fold over them in order, so a reloaded
// source rebuilds exactly the boss the stream was showing. A test frame is kept only in the
// editor preview, until the next backfill.
let events = [];
const ids = new Set();

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onBackfill((list) => {
  events = [];
  ids.clear();
  for (const e of list) remember(e);
  render(null);
});
OBSOverlay.onEvent((e) => {
  if (!e || e.replay) return;
  if (e.test && !OBSOverlay.preview) return;
  if (!remember(e)) return;
  const before = fight(events.slice(0, -1));
  render({ e, before });
});

function remember(e) {
  if (!e || !e.id || ids.has(e.id)) return false;
  ids.add(e.id);
  events.push(e);
  return true;
}

function num(key, fallback, min) {
  const n = Number(fields[key]);
  return Number.isFinite(n) && n >= min ? n : fallback;
}

/** The damage `e` deals under the current fields; 0 for a type that deals none. */
function damageOf(e) {
  if (!own(DAMAGE, e.type)) return 0;
  const d = DAMAGE[e.type];
  return num(d.field, d.fallback, 0) * d.times(e);
}

/** The boss after `list`, in order: { name, max, hp, defeats }. A hit that empties the bar
 * makes its dealer the next boss, at the first boss's HP plus the growth per defeat;
 * overkill is lost. */
function fight(list) {
  const start = num("startHp", 1000, 1);
  const growth = num("hpGrowth", 500, 0);
  const first = fields.bossName != null ? String(fields.bossName) : "The Streamer";
  let boss = { name: first, max: start, hp: start, defeats: 0 };
  for (const e of list) {
    const dmg = damageOf(e);
    if (dmg <= 0) continue;
    if (dmg >= boss.hp) {
      const defeats = boss.defeats + 1;
      const max = start + growth * defeats;
      boss = { name: e.actorName || "Someone", max, hp: max, defeats };
    } else {
      boss = { ...boss, hp: boss.hp - dmg };
    }
  }
  return boss;
}

function applyFields(f) {
  fields = f;
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  // Design px, not device px: template.css resolves it against the root scale.
  if (f.fontSize != null) set("--ov-size", String(Number(f.fontSize) || 22));
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.barColor) set("--ov-bar", String(f.barColor));
  if (f.trackColor) set("--ov-track", String(f.trackColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  render(null);
}

function fmt(n) {
  return OBSOverlay.formatCount(Math.ceil(n));
}

function replay(cls) {
  bossEl.classList.remove("hit-shake", "new-boss");
  // Read a layout value so the class removal takes effect before it is added back.
  void bossEl.offsetWidth;
  bossEl.classList.add(cls);
}

// `hit` is the event that just landed and the boss before it, or null for a rebuild (a
// backfill, a settings change), which redraws without animating. Every value lands via
// textContent, so a viewer's name cannot inject markup.
function render(hit) {
  const boss = fight(events);
  nameEl.textContent = boss.name;
  fillEl.style.width = ((boss.hp / boss.max) * 100).toFixed(2) + "%";
  hpEl.textContent = fmt(boss.hp) + " / " + fmt(boss.max) + " HP";
  if (!hit) {
    hitEl.textContent = "";
    return;
  }
  const dmg = damageOf(hit.e);
  if (dmg <= 0) return;
  const who = hit.e.actorName || "Someone";
  if (boss.defeats > hit.before.defeats) {
    hitEl.textContent = who + " defeated " + hit.before.name + "!";
    replay("new-boss");
  } else {
    hitEl.textContent = who + " hit for " + fmt(dmg);
    replay("hit-shake");
  }
}
