const el = document.getElementById("alert");
const nameEl = document.getElementById("alert-name");
const msgEl = document.getElementById("alert-msg");

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
};

let fields = {};
let queue = [];
let showing = false;

OBSOverlay.onLoad((ctx) => {
  fields = ctx.fields || {};
  if (fields.accent) document.documentElement.style.setProperty("--accent", String(fields.accent));
  if (fields.font) document.body.style.setProperty("--ov-font", String(fields.font));
});

OBSOverlay.onEvent((e) => {
  queue.push(e);
  if (!showing) next();
});

function render(tmpl, e) {
  return String(tmpl || "")
    .replaceAll("{name}", e.actorName || "Someone")
    .replaceAll("{amount}", e.amount != null ? String(e.amount) : "");
}

function next() {
  const e = queue.shift();
  if (!e) {
    showing = false;
    return;
  }
  showing = true;
  const tmpl = fields[TEMPLATE_KEY[e.type]] || "{name}";
  // The strip has to remove exactly what render() substituted for {name}, fallback
  // included -- otherwise an unnamed actor shows as "Someone Someone just followed!".
  const shownName = e.actorName || "Someone";
  nameEl.textContent = shownName;
  msgEl.textContent = render(tmpl, e).replace(shownName + " ", "");
  el.classList.add("show");
  if (fields.sound) OBSOverlay.playSound(String(fields.sound), 1);
  const durMs = (Number(fields.duration) || 5) * 1000;
  setTimeout(() => {
    el.classList.remove("show");
    setTimeout(next, 360); // let the fade-out finish before the next
  }, durMs);
}
