const root = document.getElementById("chat");

// Per-platform tag label + color (mirrors the app's MultichatDock palette). Also
// the author-color fallback source when a message has no author.color.
const PLATFORM = {
  twitch: { label: "Twitch", color: "#a970ff" },
  youtube: { label: "YouTube", color: "#ff4e45" },
  kick: { label: "Kick", color: "#53fc18" },
  facebook: { label: "Facebook", color: "#0866ff" },
};

let fields = {};
let maxMessages = 50;
let lifetimeMs = 0;

// Paid (Super Chat/Super Sticker/Cheer) chip. Strict "#RRGGBB" validation before a
// platform-supplied string reaches inline CSS -- mirrors
// frontend/web/src/lib/utils/hexColor.ts (this file is plain JS with no bundler, so
// it can't import that module and keeps its own copy; reconcile the contrast math
// there if it changes here).
const HEX_COLOR_RE = /^#[0-9a-fA-F]{6}$/;

function srgbToLinear(c) {
  const v = c / 255;
  return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
}

// Black or white, whichever contrasts more against a validated "#RRGGBB" background.
function readableTextColor(hex) {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  const l = 0.2126 * srgbToLinear(r) + 0.7152 * srgbToLinear(g) + 0.0722 * srgbToLinear(b);
  const contrastWithWhite = 1.05 / (l + 0.05);
  const contrastWithBlack = (l + 0.05) / 0.05;
  return contrastWithBlack >= contrastWithWhite ? "#000000" : "#ffffff";
}

OBSOverlay.onLoad((ctx) => applyFields(ctx.fields || {}));
OBSOverlay.onChat((m) => appendMessage(m));
// Messages have no expiry by default (messageLifetimeSec 0), so without this the last
// broadcast's chat would still be sitting on screen when the next one goes live. Clearing
// the column back to empty is the same state the widget starts in before any message has
// arrived.
OBSOverlay.onStream((s) => {
  if (s && s.active !== true) {
    root.textContent = "";
  }
});

function applyFields(f) {
  fields = f;
  const set = (k, v) => document.documentElement.style.setProperty(k, v);
  if (f.fontFamily) set("--ov-font", String(f.fontFamily));
  // Design px, not device px: template.css resolves it against the root scale.
  if (f.fontSize != null) set("--ov-size", String(Number(f.fontSize) || 20));
  if (f.textColor) set("--ov-text", String(f.textColor));
  if (f.authorDefaultColor) set("--ov-author", String(f.authorDefaultColor));
  if (f.backgroundColor) set("--ov-bg", String(f.backgroundColor));

  maxMessages = Math.max(1, Number(f.maxMessages) || 50);
  lifetimeMs = Math.max(0, Number(f.messageLifetimeSec) || 0) * 1000;

  const anim = f.animation === "none" ? "" : f.animation === "slide" ? "anim-slide" : "anim-fade";
  document.body.className = anim;
}

function appendMessage(m) {
  if (!m || !m.author) return;
  // hasOwnProperty, not a bare lookup: a platform string like "constructor" would
  // otherwise resolve through Object.prototype to a function, not undefined, and
  // `plat.color` below would then be undefined instead of falling back.
  const plat = Object.prototype.hasOwnProperty.call(PLATFORM, m.platform)
    ? PLATFORM[m.platform]
    : { label: m.platform || "", color: "#888888" };

  const row = document.createElement("div");
  row.className = "msg";

  // Platform tag (opt-in).
  if (fields.showPlatform) {
    const tag = document.createElement("span");
    tag.className = "platform";
    tag.style.background = plat.color;
    tag.textContent = plat.label;
    row.appendChild(tag);
  }

  // Badges (opt-in, image-only -- text-fallback badges are skipped for a clean look).
  if (fields.showBadges && Array.isArray(m.author.badges)) {
    for (const b of m.author.badges) {
      if (!b || !b.url) continue;
      const img = document.createElement("img");
      img.className = "badge";
      img.src = b.url;
      img.alt = b.kind || "";
      img.title = b.kind || "";
      img.draggable = false;
      row.appendChild(img);
    }
  }

  // Author name in its color, falling back to the platform / themed default.
  const author = document.createElement("span");
  author.className = "author";
  author.style.color = m.author.color || plat.color || "";
  author.textContent = m.author.name || "";
  row.appendChild(author);

  const frags = Array.isArray(m.fragments) ? m.fragments : [];
  const paid = m.paid && typeof m.paid.amount === "string" ? m.paid : null;

  // The colon only ever separated the author from the body, so a paid line with no
  // comment (a Super Chat with just an amount) skips it rather than leaving it
  // pointing at nothing.
  if (frags.length > 0 || !paid) {
    const sep = document.createElement("span");
    sep.className = "sep";
    sep.textContent = ":";
    row.appendChild(sep);
  }

  // Amount chip: `paid.amount` is the platform's own display string, rendered
  // verbatim via textContent (never innerHTML). `paid.color` (YouTube's tier color)
  // drives the chip/row accent when present and valid; a cheer (Twitch Bits) never
  // carries one, so the fallback is the message's own platform color -- `plat.color`,
  // already computed above -- rather than a fixed color, since a hardcoded YouTube
  // red would mislabel a Twitch cheer as YouTube money. `plat.color` is re-validated
  // here rather than trusted as-is (nothing upstream guarantees the PLATFORM table
  // stays literal hex forever), so `color` is always a real 6-digit "#RRGGBB" and
  // --paid is always set to one.
  if (paid) {
    row.classList.add("paid");
    const platColor = HEX_COLOR_RE.test(plat.color) ? plat.color : "#888888";
    const color = typeof paid.color === "string" && HEX_COLOR_RE.test(paid.color) ? paid.color : platColor;
    row.style.setProperty("--paid", color);
    const chip = document.createElement("span");
    chip.className = "amount";
    chip.style.color = readableTextColor(color);
    chip.textContent = paid.amount;
    row.appendChild(chip);
  }

  // Message body from fragments. Text -> escaped text node (never innerHTML);
  // emote -> <img>. Unknown fragment types are ignored.
  if (frags.length > 0) {
    const body = document.createElement("span");
    body.className = "body";
    for (const frag of frags) {
      if (!frag) continue;
      if (frag.type === "emote" && frag.url) {
        const img = document.createElement("img");
        img.className = "emote";
        img.src = frag.url;
        img.alt = frag.code || "";
        img.title = frag.code || "";
        img.draggable = false;
        body.appendChild(img);
      } else if (frag.text != null) {
        body.appendChild(document.createTextNode(String(frag.text)));
      }
    }
    row.appendChild(body);
  }

  root.appendChild(row);

  // Cap the DOM: drop oldest rows beyond the limit.
  while (root.childElementCount > maxMessages) {
    root.removeChild(root.firstElementChild);
  }

  // Keep newest in view (matters once the column overflows).
  root.scrollTop = root.scrollHeight;

  // Optional lifetime: fade out then remove.
  if (lifetimeMs > 0) {
    setTimeout(() => {
      row.classList.add("leaving");
      setTimeout(() => {
        if (row.parentNode === root) root.removeChild(row);
      }, 450);
    }, lifetimeMs);
  }
}
