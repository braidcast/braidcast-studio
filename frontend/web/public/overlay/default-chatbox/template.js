const root = document.getElementById("chat");

// Per-platform tag label + color (mirrors the app's MultichatDock palette). Also
// the author-color fallback source when a message has no author.color.
const PLATFORM = {
  twitch: { label: "Twitch", color: "#a970ff" },
  youtube: { label: "YouTube", color: "#ff4e45" },
  kick: { label: "Kick", color: "#53fc18" },
};

let fields = {};
let maxMessages = 50;
let lifetimeMs = 0;

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
  const plat = PLATFORM[m.platform] || { label: m.platform || "", color: "#888" };

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

  const sep = document.createElement("span");
  sep.className = "sep";
  sep.textContent = ":";
  row.appendChild(sep);

  // Message body from fragments. Text -> escaped text node (never innerHTML);
  // emote -> <img>. Unknown fragment types are ignored.
  const body = document.createElement("span");
  body.className = "body";
  const frags = Array.isArray(m.fragments) ? m.fragments : [];
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
