const rowsEl = document.getElementById("rows");

// Per-platform accent + the field key that overrides it, keyed by the platform string the
// host sends. Adding a platform is a row here, not a new branch.
const PLATFORMS = {
  twitch: { colorKey: "colorTwitch", color: "#a970ff" },
  youtube: { colorKey: "colorYouTube", color: "#ff4e45" },
  kick: { colorKey: "colorKick", color: "#53fc18" },
};

// Every key the table has no row for. Explicit rather than a lookup that can return
// undefined, so an unrecognized platform still gets a dot instead of throwing mid-render.
const OTHER = { colorKey: "colorOther", color: "#9aa0a6" };

// Stable order for chatters tied on count and first-seen -- unreachable while `seq` is
// unique, but it keeps the comparator total rather than relying on sort stability.
const PLATFORM_ORDER = ["twitch", "youtube", "kick"];

// One row per metric: how a chatter's figure is read out of its record. Only messages
// exists today; a second one is a row here plus its dropdown option, not a branch in
// render(). `stamps` is already pruned to the window when there is one, so its length IS
// the windowed count.
const METRICS = {
  messages: (c, windowed) => (windowed ? c.stamps.length : c.count),
};

// The align choice as both a text-align keyword and its flex equivalent.
const ALIGN = {
  left: { text: "left", flex: "flex-start" },
  center: { text: "center", flex: "center" },
  right: { text: "right", flex: "flex-end" },
};

// Retention caps. A browser source sits in a scene for the whole broadcast, so neither the
// chatter table nor one chatter's timestamp list may grow with time.
//
// Only the top ten rows can ever be drawn, so a chatter ranked below the 500th by messages
// cannot reach the board and dropping it loses nothing that could be shown. Pruning runs
// at the high-water mark and trims back to the cap, so the sort it costs happens once per
// hundred new chatters rather than once per message past the cap.
const MAX_CHATTERS = 500;
const PRUNE_AT = 600;
// Timestamps exist only so a rolling window can expire them; the window prune drops them
// from the front every recount, so this cap binds only during a flood. 600 is ten messages
// a second for a solid minute from a single chatter. The table's worst case is therefore
// bounded at MAX_CHATTERS * MAX_STAMPS timestamps regardless of how long the stream runs.
const MAX_STAMPS = 600;

// Chat arrives in bursts -- a raid lands hundreds of messages in a second -- and a browser
// source that relayouts per message stalls the scene it is composited into. Rendering is
// coalesced onto this cadence, so the cost is bounded by time rather than by chat volume.
const RENDER_MS = 250;

// Every counted message is kept as a line, { identity, stamp, chatter, gone, sameId }, so a
// moderator's delete or ban can take it back off the board. The identity is what an op is
// matched on (OBSOverlay.chatIdentity -- never the text). A ban has to reach every message the
// op names, and the chatters most worth removing are the ones with the most messages, so
// lines are held up to this many across all chatters. Past it the oldest lines are evicted
// in batches of EVICT_SLACK, each folded into its chatter's per-destination tally (see
// foldEvicted), which a ban still takes back whole, so the bound costs precision only for
// `beforeTs` bans over evicted lines, and for a delete of an evicted line, which no longer
// finds it and leaves it counting; never a banned chatter left on the board. Every browser
// source is its own renderer, so the bound is kept small: a held line measures about 360
// bytes on V8 without pointer compression, its message and author id strings included, so
// about 8 MB at worst (MAX_LINES + EVICT_SLACK lines), and less in Chromium, whose pointer
// compression shrinks the objects though not the strings.
const MAX_LINES = 20000;
const EVICT_SLACK = 2000;

let fields = {};
// key -> { key, platform, name, count, seq, stamps, lines, evicted }. See keyFor for what
// identifies a chatter. `count` is always lines.length plus every evicted tally's count.
const chatters = new Map();
// Every live line, oldest first from `orderHead`; a line taken back by moderation or dropped
// with its chatter is marked `gone` and skipped, and compactOrder reclaims the slots.
const order = [];
let orderHead = 0;
let liveLines = 0;
// Message id -> its newest live line, older lines with the same id chained through `sameId`
// (one message read by two of our accounts). A delete names only an id, so this is what keeps
// it from scanning every held line.
const linesById = new Map();
// Arrival order of first message, the tie-break for equal counts. A counter rather than a
// timestamp: message `ts` comes from the platform and can arrive out of order, which would
// make "earliest first seen" swap between renders -- the exact reshuffle it prevents.
let seq = 0;
let dirty = false;
let renderTimer = null;
// Running only while a rolling window is configured. Whole-session mode has nothing that
// expires with time, so it needs no tick of its own.
let windowTimer = null;

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onChat((m) => note(m));
OBSOverlay.onChatModeration((op, removes) => uncount(op, removes));
// A rolling window ages chatters out on its own, but the tally itself has nothing that
// expires with time, so it would otherwise carry the previous broadcast's standings into
// the next one. Clearing back to an empty table is the same "nobody has reported yet"
// state the widget starts in and already draws as nothing.
// A frame the editor's Test button sent reaches this page on stream too. What it carries is
// kept only in the editor preview: on stream it would stand as the real state until the next
// real frame, which can be minutes away.
const offStreamTest = (frame) => !!(frame && frame.test && !OBSOverlay.preview);

OBSOverlay.onStream((s) => {
  if (s && s.active !== true && !offStreamTest(s)) {
    chatters.clear();
    order.length = 0;
    orderHead = 0;
    liveLines = 0;
    linesById.clear();
    render();
  }
});

function applyFields(f) {
  fields = f;
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  // Design px, not device px: template.css resolves it against the root scale.
  if (f.fontSize != null) set("--ov-size", String(Number(f.fontSize) || 22));
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));
  if (f.gap != null) set("--ov-gap", String(Math.max(0, Number(f.gap) || 0)));

  const column = String(f.orientation || "column") === "column";
  const align = ALIGN[String(f.align || "left")] || ALIGN.left;
  set("--ov-dir", column ? "column" : "row");
  set("--ov-align", align.text);
  // In a row the entries want vertical centering; in a column the align choice becomes the
  // cross axis, so it drives align-items instead of only text-align.
  set("--ov-cross", column ? align.flex : "center");

  syncWindowTimer();
  render();
}

// Checkbox reader with a per-field fallback for when the key is missing entirely (an
// older widget, or the user deleted the field). One predicate rather than a truthiness
// test per call site, so the toggles cannot drift apart from each other.
function isOn(key, fallback) {
  const v = fields[key];
  if (v == null) {
    return fallback;
  }
  return v === true || v === "true";
}

// An empty string the user typed deliberately is a valid answer, so only a missing key
// falls back.
function textField(key, fallback) {
  const v = fields[key];
  return v != null ? String(v) : fallback;
}

function numberField(key, fallback, min, max) {
  const n = Number(fields[key]);
  if (!Number.isFinite(n)) {
    return fallback;
  }
  return Math.min(max, Math.max(min, Math.round(n)));
}

function accentFor(platform) {
  const p = Object.prototype.hasOwnProperty.call(PLATFORMS, platform) ? PLATFORMS[platform] : OTHER;
  return fields[p.colorKey] ? String(fields[p.colorKey]) : p.color;
}

function orderOf(platform) {
  const i = PLATFORM_ORDER.indexOf(platform);
  return i === -1 ? PLATFORM_ORDER.length : i;
}

// 0 means the whole session; anything else is the rolling window in minutes.
function windowMs() {
  const n = Number(fields.windowMinutes);
  return Number.isFinite(n) && n > 0 ? n * 60000 : 0;
}

// A chatter is the platform plus the sender's stable per-user id, and only the display name
// when the platform sent no id. The id is what makes the tally right: a display name changes
// mid-stream (one person would split into two rows) and two people on one platform may hold
// the same name (two people would merge into one row). The account id on the message names
// OUR receiving account, not the sender, so it stays out of the key either way -- one chatter
// writing to two of our channels is one person.
//
// The middle segment is the namespace tag, and it is what keeps the two schemes apart: an
// id-keyed chatter always reads "i" there and a name-keyed one always reads "n", so no id and
// no name -- whatever either contains, newlines included -- can ever produce the same key
// string. A missing id therefore falls back to the name rather than keying everyone who lacks
// one on a shared "".
//
// The name is not case-folded: Twitch sends one canonical casing per user so folding buys
// nothing there, while on YouTube and Kick two different people may hold names that differ
// only in case and folding would merge them.
function keyFor(platform, id, name) {
  return id ? platform + "\ni\n" + id : platform + "\nn\n" + name;
}

function note(m) {
  if (!m || !m.author) {
    return;
  }
  const name = typeof m.author.name === "string" ? m.author.name.trim() : "";
  // A message with no author name cannot be attributed to anybody, so it counts for
  // nobody rather than accruing to a blank row.
  if (!name) {
    return;
  }
  const platform = platformOf(m.platform);
  const ts = typeof m.ts === "number" && Number.isFinite(m.ts) ? m.ts : Date.now();

  const key = keyFor(platform, groupId(m.author.id), name);
  let c = chatters.get(key);
  if (c === undefined) {
    c = { key: key, platform: platform, name: name, count: 0, seq: seq++, stamps: [], lines: [], evicted: new Map() };
    chatters.set(key, c);
  }
  // An id-keyed row survives a rename, so the board shows the name they go by now rather
  // than whichever one they happened to arrive under.
  c.name = name;
  c.count += 1;
  c.stamps.push(ts);
  const line = { identity: OBSOverlay.chatIdentity(m), stamp: ts, chatter: c, gone: false, sameId: null };
  c.lines.push(line);
  order.push(line);
  liveLines += 1;
  const msgId = line.identity.id;
  if (typeof msgId === "string" && msgId) {
    line.sameId = linesById.get(msgId) || null;
    linesById.set(msgId, line);
  }
  if (c.stamps.length > MAX_STAMPS) {
    c.stamps.splice(0, c.stamps.length - MAX_STAMPS);
  }
  if (liveLines > MAX_LINES + EVICT_SLACK) {
    evictOldest();
  }
  if (chatters.size > PRUNE_AT) {
    pruneChatters();
  }

  markDirty();
}

// The platform as a key segment: an op and a message from one destination normalize alike.
function platformOf(p) {
  return typeof p === "string" ? p.trim().toLowerCase() : "";
}

// The sender id as a key segment, trimmed so one person is one row. Grouping only: a
// moderation op is matched on the raw id the line's identity keeps, the way the host and the
// dock match it, and uncount finds the row through this same function.
function groupId(id) {
  return typeof id === "string" ? id.trim() : "";
}

// A removed message stops counting. A delete takes its message off its author's count, and a
// ban or timeout takes off every message of theirs the op names: only those from the op's
// own destination, admitted and said before it, so what they said on another channel or
// after a timeout ran out still counts. That includes lines already evicted (MAX_LINES),
// through their per-destination tally. A row whose count reaches zero leaves the board. A
// clear of the whole chat leaves the standings alone.
function uncount(op, removes) {
  if (!op || op.action === "all") {
    return;
  }
  let changed = false;
  if (op.action === "message") {
    let line = typeof op.msgId === "string" ? linesById.get(op.msgId) : undefined;
    while (line) {
      const next = line.sameId;
      if (removes(line.identity)) {
        const c = line.chatter;
        const at = c.lines.lastIndexOf(line);
        if (at >= 0) c.lines.splice(at, 1);
        takeBack(c, line);
        changed = true;
      }
      line = next;
    }
  } else if (op.action === "user") {
    const c = chatters.get(keyFor(platformOf(op.platform), groupId(op.authorId), ""));
    if (c !== undefined) {
      const kept = [];
      for (const line of c.lines) {
        if (removes(line.identity)) {
          takeBack(c, line);
          changed = true;
        } else {
          kept.push(line);
        }
      }
      c.lines = kept;
      for (const [dest, tally] of c.evicted) {
        if (removes(tally)) {
          c.evicted.delete(dest);
          c.count -= tally.count;
          changed = true;
        }
      }
      if (c.count <= 0) {
        chatters.delete(c.key);
      }
    }
  }
  if (changed) {
    compactOrder();
    markDirty();
  }
}

// `line`, already out of c.lines, stops counting for `c`.
function takeBack(c, line) {
  retire(line);
  c.count -= 1;
  const at = c.stamps.lastIndexOf(line.stamp);
  if (at !== -1) {
    c.stamps.splice(at, 1);
  }
  if (c.count <= 0) {
    chatters.delete(c.key);
  }
}

// `line` leaves the held set: marked gone for `order`, and unlinked from linesById.
function retire(line) {
  line.gone = true;
  liveLines -= 1;
  const msgId = line.identity.id;
  if (typeof msgId !== "string" || !msgId) {
    return;
  }
  let at = linesById.get(msgId);
  if (at === line) {
    if (line.sameId) {
      linesById.set(msgId, line.sameId);
    } else {
      linesById.delete(msgId);
    }
    return;
  }
  while (at && at.sameId !== line) {
    at = at.sameId;
  }
  if (at) {
    at.sameId = line.sameId;
  }
}

// Evict the oldest lines back down to MAX_LINES. They are each chatter's oldest too, so they
// leave c.lines as one prefix per chatter.
function evictOldest() {
  const evictedFrom = new Map();
  while (liveLines > MAX_LINES && orderHead < order.length) {
    const line = order[orderHead++];
    if (line.gone) {
      continue;
    }
    retire(line);
    foldEvicted(line.chatter, line.identity);
    evictedFrom.set(line.chatter, (evictedFrom.get(line.chatter) || 0) + 1);
  }
  for (const [c, n] of evictedFrom) {
    c.lines.splice(0, n);
  }
  compactOrder();
}

// An evicted line still counts, so it is kept as a tally per destination and raw author id:
// how many, and the latest admission seq and time among them. The tally has an identity's
// shape with those maxima in it, so one `removes` test answers for all of its lines at once:
// if the op names the latest, it names every earlier one too. A ban with no `beforeTs` names
// every line on its destination admitted before it, which evicted lines always are, so it
// takes the whole tally; one with a `beforeTs` takes it only when all of it was said by then
// (an evicted line with no time goes with the rest, where a held one would stay).
// The stamps of an evicted line stay in c.stamps until the window or MAX_STAMPS drops them,
// and a ban does not take them back, so a row that survives a ban -- one with lines on
// another destination -- may briefly over-read its windowed count by lines evicted inside
// the window, which takes over MAX_LINES messages within one window.
function foldEvicted(c, identity) {
  const authorId = identity.author.id;
  const dest = identity.accountId + "\n" + (identity.profileUuid || "") + "\n" + (authorId || "");
  let tally = c.evicted.get(dest);
  if (tally === undefined) {
    tally = {
      accountId: identity.accountId,
      profileUuid: identity.profileUuid,
      id: "",
      seq: 0,
      ts: 0,
      author: { id: authorId },
      count: 0,
    };
    c.evicted.set(dest, tally);
  }
  tally.count += 1;
  tally.seq = Math.max(tally.seq, Number(identity.seq) || 0);
  tally.ts = Math.max(tally.ts, Number(identity.ts) || 0);
}

// Reclaim the slots of evicted and gone lines once they are at least half of `order`, so it
// stays within a small multiple of the live count at an amortized constant cost per line.
function compactOrder() {
  if (orderHead * 2 < order.length && order.length <= 2 * liveLines + EVICT_SLACK) {
    return;
  }
  let w = 0;
  for (let r = orderHead; r < order.length; r++) {
    if (!order[r].gone) {
      order[w++] = order[r];
    }
  }
  order.length = w;
  orderHead = 0;
}

function markDirty() {
  dirty = true;
  if (renderTimer === null) {
    renderTimer = setTimeout(flush, RENDER_MS);
  }
}

// Trim the table back to the cap, keeping the chatters with the most messages, so the row
// dropped is never one that could still be drawn. Lifetime count rather than the windowed
// figure: it is the only measure that survives the window moving, and the two only disagree
// past five hundred distinct chatters.
function pruneChatters() {
  const all = Array.from(chatters.keys()).sort((a, b) => compare(chatters.get(a), chatters.get(b)));
  for (let i = MAX_CHATTERS; i < all.length; i++) {
    for (const line of chatters.get(all[i]).lines) {
      retire(line);
    }
    chatters.delete(all[i]);
  }
  compactOrder();
}

// Count descending, then earliest first seen. Equal counts are the normal case on a board
// of five, so without the second key two rows would trade places on every render.
function compare(a, b) {
  return b.count - a.count || a.seq - b.seq || orderOf(a.platform) - orderOf(b.platform);
}

function flush() {
  renderTimer = null;
  if (!dirty) {
    return;
  }
  dirty = false;
  render();
}

// A rolling window expires rows with time and not only with new messages, so it needs a
// tick of its own -- otherwise the last chatter before a quiet spell stays on the board
// after their messages have aged out.
function syncWindowTimer() {
  const want = windowMs() > 0;
  if (want && windowTimer === null) {
    windowTimer = setInterval(() => render(), 1000);
  } else if (!want && windowTimer !== null) {
    clearInterval(windowTimer);
    windowTimer = null;
  }
}

// Stamps are pushed in arrival order, so everything outside the window sits at the front:
// dropping that prefix both bounds retention and makes the remaining length the windowed
// count, with no per-render scan of the whole list.
function pruneStamps(c, cutoff) {
  let i = 0;
  while (i < c.stamps.length && c.stamps[i] < cutoff) {
    i += 1;
  }
  if (i > 0) {
    c.stamps.splice(0, i);
  }
}

// Every value lands via textContent, so a display name cannot inject markup.
function makeRow(entry, rank) {
  const row = document.createElement("span");
  row.className = "row";

  if (isOn("showRank", true)) {
    const el = document.createElement("span");
    el.className = "rank";
    el.textContent = rank + ".";
    row.appendChild(el);
  }
  if (isOn("showPlatform", true)) {
    const dot = document.createElement("span");
    dot.className = "dot";
    dot.style.background = accentFor(entry.platform);
    row.appendChild(dot);
  }

  const name = document.createElement("span");
  name.className = "name";
  name.textContent = entry.name;
  row.appendChild(name);

  if (isOn("showCount", true)) {
    const count = document.createElement("span");
    count.className = "count";
    count.textContent = entry.count.toLocaleString();
    row.appendChild(count);
  }

  return row;
}

function render() {
  const ms = windowMs();
  const cutoff = ms > 0 ? Date.now() - ms : 0;
  const metric = METRICS[String(fields.metric || "messages")] || METRICS.messages;
  // A floor below one would admit chatters whose messages have all aged out of the window,
  // which is a name on the board with nothing behind it.
  const min = numberField("minMessages", 1, 1, Number.MAX_SAFE_INTEGER);
  const topN = numberField("topN", 5, 1, 10);

  const entries = [];
  for (const c of chatters.values()) {
    if (cutoff > 0) {
      pruneStamps(c, cutoff);
    }
    const count = metric(c, cutoff > 0);
    if (count < min) {
      continue;
    }
    entries.push({ platform: c.platform, name: c.name, count: count, seq: c.seq });
  }
  entries.sort(compare);

  rowsEl.textContent = "";
  const shown = Math.min(entries.length, topN);
  for (let i = 0; i < shown; i++) {
    rowsEl.appendChild(makeRow(entries[i], i + 1));
  }

  // Nobody has met the threshold yet. An empty board draws nothing unless the user asked
  // for a stand-in, and even then it is their own words rather than a fabricated zero.
  if (shown === 0) {
    const text = textField("emptyText", "");
    if (text) {
      const el = document.createElement("span");
      el.className = "row empty";
      el.textContent = text;
      rowsEl.appendChild(el);
    }
  }

  rowsEl.hidden = rowsEl.childElementCount === 0;
}
