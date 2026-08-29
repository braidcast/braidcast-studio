// Per-slot text styling: the property vocabulary, its sanitizers, and the CSS a slot's
// stored values compile to.
//
// A widget marks a styleable element with data-ov-slot="<name>" and declares one
// `textstyle` field keyed `style_<name>`. The runtime compiles every such field into one
// rule per slot, so adding a slot to a future widget is an HTML attribute plus a
// fields.json row -- no per-widget JS and nothing to register here. That row must declare
// `"default": {}`: the whole style object lives under one key, so a default carrying
// values would be copied wholesale into the widget by its first edit. The panel refuses
// any other default rather than let that happen quietly -- textStyleDefaultFault in
// fieldTypes.ts carries the full reasoning.
//
// ONE table drives both directions: the runtime reads it to emit CSS, and the fields
// panel's textstyle control reads it to render the editor. So every property the editor
// offers compiles to something, and every property that compiles is reachable -- with one
// deliberate exception, the DERIVED declarations below, which have no row of their own
// because a property the user DID set is what implies them.
//
// "Compiles to something" is an invariant worth naming, because CSS breaks it easily: a
// stroke color rides an initial width of 0, a border width rides an initial style of none,
// a shadow with no offset and no blur hides behind the glyph. Each is a property that is
// legal, storable, and paints nothing -- so a row would light its marker over a stream
// that never changed. A property that needs a companion belongs in COMPOSITES, which
// declares the pairing once and lets the editor supply the missing half (dependencyFor)
// and drop the orphans (dependentsOf) without either side naming a property. DERIVED is
// for the narrower case where the companion has no row and one fixed value will always
// do. Adding a companion pair as two independent entries is the mistake to avoid.
//
// PRECEDENCE, per property: a slot style beats the widget-wide field covering the same
// ground, and only for the properties it actually sets -- a slot that names a color keeps
// the widget's font. This falls out of where each lands rather than out of a rule anyone
// has to enforce: the widget-wide fields drive custom properties the widget's CONTAINER
// rule reads, while a slot rule declares on the styled element itself, and a declaration
// on a descendant beats an inherited one whatever the specificity. Inheritance is also
// what bounds the vocabulary: the container's background and text-align are not inherited,
// so a slot cannot override them. Background is still offered, because a slot background
// is a useful second box painted INSIDE the container's rather than a replacement for it.
// Alignment is not offered at all -- a slot is an inline element with no width of its own,
// so text-align would compile to a declaration with nothing to align; the widget's own
// Align field remains the only way to position the text.
//
// Every value is user-supplied, so nothing is interpolated into CSS raw. Numbers are
// coerced, clamped, rounded and formatted fixed-point (so they can only ever be digits, a
// dot and a sign);
// enums must be a member of their own option list; colors and font stacks must match an
// allowlist whose character set excludes every delimiter that could close a declaration
// or a rule. A value that fails simply does not reach the stylesheet.

import { clamp } from "../lib/utils/clamp";
import { isPlainObject } from "../lib/utils/plainObject";

/** The editor's tab strip, in display order, with the heading each carries. One table, so
 * a group cannot gain properties without gaining a tab to reach them through. */
export const STYLE_GROUPS = [
  { id: "typography", label: "Type" },
  { id: "fill", label: "Fill" },
  { id: "box", label: "Box" },
  { id: "effects", label: "Effects" },
] as const;
export type StyleGroup = (typeof STYLE_GROUPS)[number]["id"];

export interface StyleOption {
  value: string;
  label: string;
}

interface StyleBase {
  key: string;
  label: string;
  group: StyleGroup;
  /** The CSS property this declares on its own, or null when the property only
   * contributes to a composite value that COMPOSITES assembles from several keys. */
  css: string | null;
  /** The value the editor writes when it has to supply this property itself: a color
   * row's "Set ..." button, and any composite gate whose part a user reaches first. Not
   * every kind carries one -- a property without a seed can only be set by hand. */
  seed?: string | number;
}

export type StyleProp = StyleBase &
  (
    | { kind: "length"; unit: "px" | "em"; min: number; max: number; step: number }
    | { kind: "ratio"; min: number; max: number; step: number }
    | { kind: "percent"; min: number; max: number; step: number }
    | { kind: "color"; seed: string }
    | { kind: "enum"; options: StyleOption[] }
    | { kind: "font" }
  );

const WEIGHTS: StyleOption[] = [100, 200, 300, 400, 500, 600, 700, 800, 900].map((n) => ({
  value: String(n),
  label: String(n),
}));

/** Order is display order within a group. `key` is also the stored key, so renaming one
 * drops whatever a user had set under the old name -- treat them as a contract. */
export const STYLE_PROPS: StyleProp[] = [
  { key: "fontFamily", label: "Font", group: "typography", css: "font-family", kind: "font" },
  {
    key: "fontSize",
    label: "Size",
    group: "typography",
    css: "font-size",
    kind: "length",
    unit: "px",
    min: 4,
    max: 400,
    step: 1,
  },
  { key: "fontWeight", label: "Weight", group: "typography", css: "font-weight", kind: "enum", options: WEIGHTS },
  {
    key: "fontStyle",
    label: "Italic",
    group: "typography",
    css: "font-style",
    kind: "enum",
    options: [
      { value: "normal", label: "Upright" },
      { value: "italic", label: "Italic" },
    ],
  },
  {
    key: "letterSpacing",
    label: "Letter Spacing",
    group: "typography",
    css: "letter-spacing",
    kind: "length",
    // em rather than px so the tracking a user dials in survives a font-size change.
    unit: "em",
    min: -0.5,
    max: 2,
    step: 0.01,
  },
  {
    key: "lineHeight",
    label: "Line Height",
    group: "typography",
    css: "line-height",
    kind: "ratio",
    min: 0.5,
    max: 4,
    step: 0.05,
  },
  {
    key: "textTransform",
    label: "Case",
    group: "typography",
    css: "text-transform",
    kind: "enum",
    options: [
      { value: "none", label: "As typed" },
      { value: "uppercase", label: "UPPERCASE" },
      { value: "lowercase", label: "lowercase" },
      { value: "capitalize", label: "Capitalize" },
    ],
  },

  { key: "color", label: "Text Color", group: "fill", css: "color", kind: "color", seed: "#ffffff" },
  { key: "opacity", label: "Opacity", group: "fill", css: "opacity", kind: "percent", min: 0, max: 100, step: 1 },

  {
    key: "backgroundColor",
    label: "Background",
    group: "box",
    css: "background-color",
    kind: "color",
    seed: "rgba(0,0,0,0.6)",
  },
  {
    key: "paddingX",
    label: "Padding X",
    group: "box",
    css: "padding-inline",
    kind: "length",
    unit: "px",
    min: 0,
    max: 200,
    step: 1,
  },
  {
    key: "paddingY",
    label: "Padding Y",
    group: "box",
    css: "padding-block",
    kind: "length",
    unit: "px",
    min: 0,
    max: 200,
    step: 1,
  },
  {
    key: "borderRadius",
    label: "Corner Radius",
    group: "box",
    css: "border-radius",
    kind: "length",
    unit: "px",
    min: 0,
    max: 200,
    step: 1,
  },
  {
    key: "borderWidth",
    label: "Border Width",
    group: "box",
    css: "border-width",
    kind: "length",
    unit: "px",
    min: 0,
    max: 40,
    step: 1,
  },
  {
    key: "borderStyle",
    label: "Border Style",
    group: "box",
    css: "border-style",
    kind: "enum",
    options: [
      { value: "solid", label: "Solid" },
      { value: "dashed", label: "Dashed" },
      { value: "dotted", label: "Dotted" },
      { value: "double", label: "Double" },
      { value: "none", label: "None" },
    ],
  },
  { key: "borderColor", label: "Border Color", group: "box", css: "border-color", kind: "color", seed: "#ffffff" },

  {
    key: "outlineWidth",
    label: "Outline Width",
    group: "effects",
    css: null,
    kind: "length",
    unit: "px",
    // Floored above zero rather than at it: a zero-width stroke paints nothing, and
    // "no outline at all" is what the row's own clear button already expresses.
    min: 0.5,
    max: 20,
    step: 0.5,
    seed: 2,
  },
  {
    key: "outlineColor",
    label: "Outline Color",
    group: "effects",
    css: null,
    kind: "color",
    seed: "#000000",
  },
  {
    key: "shadowColor",
    label: "Shadow Color",
    group: "effects",
    css: null,
    kind: "color",
    seed: "rgba(0,0,0,0.6)",
  },
  {
    key: "shadowX",
    label: "Shadow X",
    group: "effects",
    css: null,
    kind: "length",
    unit: "px",
    min: -50,
    max: 50,
    step: 1,
  },
  {
    key: "shadowY",
    label: "Shadow Y",
    group: "effects",
    css: null,
    kind: "length",
    unit: "px",
    min: -50,
    max: 50,
    step: 1,
  },
  {
    key: "shadowBlur",
    label: "Shadow Blur",
    group: "effects",
    css: null,
    kind: "length",
    unit: "px",
    min: 0,
    max: 100,
    step: 1,
  },
];

const PROP_BY_KEY = new Map(STYLE_PROPS.map((p) => [p.key, p]));

/** Properties whose keys only mean something together, assembled into one declaration.
 * This is also the mechanism that keeps the editor honest: a part on its own compiles to
 * nothing, so `dependencyFor` below has the editor supply the gate rather than let a row
 * light its marker over a stream that never changes. Companion pairs belong here for that
 * reason, not only multi-value shorthands. */
interface Composite {
  css: string;
  /** The key that must be present before the declaration is emitted at all. */
  gate: string;
  parts: { key: string; fallback: string }[];
  /** Some combinations of legal part values still paint nothing. When every key in
   * `whenAllZero` resolves to a zero length, `part` takes `value` instead, so a gate the
   * user has set always shows. Resolved values are compared, not stored ones -- an absent
   * key contributes its fallback and counts the same as one explicitly set to 0. */
  rescue?: { whenAllZero: string[]; part: string; value: string };
}

const COMPOSITES: Composite[] = [
  {
    css: "text-shadow",
    // Without a color there is nothing to cast, and an offset alone would render the
    // inherited color at an offset -- which reads as a rendering fault, not a shadow.
    gate: "shadowColor",
    parts: [
      { key: "shadowX", fallback: "0" },
      { key: "shadowY", fallback: "0" },
      { key: "shadowBlur", fallback: "0" },
      { key: "shadowColor", fallback: "" },
    ],
    // A shadow with no offset AND no blur sits exactly behind the glyph and is invisible
    // under opaque text. Offset or blur alone is legitimate -- 6px 6px 0 is a hard-edged
    // drop shadow -- so only the all-zero case is rescued, with a radius rather than an
    // offset because a blur reads the same in every direction.
    rescue: { whenAllZero: ["shadowX", "shadowY", "shadowBlur"], part: "shadowBlur", value: "2px" },
  },
  {
    css: "-webkit-text-stroke",
    // The width, not the color: the stroke color's initial value is currentColor, so a
    // width alone paints a stroke in the text color and is a complete answer -- while a
    // color alone rides an initial width of 0 and paints nothing.
    gate: "outlineWidth",
    parts: [
      { key: "outlineWidth", fallback: "0" },
      { key: "outlineColor", fallback: "currentColor" },
    ],
  },
];

/** Declarations a slot gets for free once it sets something that implies them, emitted
 * ahead of the explicit ones so a user's own choice for the same property still wins. */
interface Derived {
  css: string;
  value: string;
  gateAny: string[];
}

const DERIVED: Derived[] = [
  // A width with no style is the initial `none`, which draws nothing -- so asking for a
  // border and getting no border is the default outcome without this.
  { css: "border-style", value: "solid", gateAny: ["borderWidth", "borderColor"] },
  // Keeps the stroke behind the glyph rather than eating into it from both sides.
  { css: "paint-order", value: "stroke fill", gateAny: ["outlineWidth"] },
  // Vertical padding, a radius and a border only describe a box, and a bare inline slot
  // has none. Gated on the box properties alone: a slot that only recolors its text must
  // not silently change how it flows.
  {
    css: "display",
    value: "inline-block",
    gateAny: ["backgroundColor", "paddingX", "paddingY", "borderRadius", "borderWidth", "borderStyle", "borderColor"],
  },
];

/** The key `key` needs present before it can affect anything, together with the value to
 * give that key when it is missing. Setting a shadow offset with no shadow color compiles
 * to nothing at all (see COMPOSITES), so the editor supplies the color rather than lighting
 * a row that does not reach the stream. A gate declaring no seed cannot be supplied this
 * way and answers null. */
export function dependencyFor(key: string): { key: string; value: string | number } | null {
  for (const c of COMPOSITES) {
    if (c.gate === key || !c.parts.some((p) => p.key === key)) {
      continue;
    }
    const gate = PROP_BY_KEY.get(c.gate);
    if (gate && gate.seed !== undefined) {
      return { key: c.gate, value: gate.seed };
    }
  }
  return null;
}

/** The keys that stop meaning anything once `key` is cleared -- the other side of
 * dependencyFor, so clearing a shadow color cannot leave three offsets reading as set
 * while the stream shows no shadow. */
export function dependentsOf(key: string): string[] {
  const out: string[] = [];
  for (const c of COMPOSITES) {
    if (c.gate === key) {
      out.push(...c.parts.map((p) => p.key).filter((k) => k !== key));
    }
  }
  return out;
}

// --- sanitizers ----------------------------------------------------------------------

/** #rgb, #rgba, #rrggbb, #rrggbbaa. */
const HEX_COLOR = /^#(?:[0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/i;
/** rgb()/rgba()/hsl()/hsla() whose arguments are digits, separators and percent signs
 * only. The character class is the guard: it admits no quote, parenthesis, semicolon,
 * brace or comment marker, so the value cannot escape the declaration it lands in. */
const FUNC_COLOR = /^(?:rgba?|hsla?)\([\d.,%\s/+-]{1,64}\)$/i;
/** A bare CSS color keyword (`transparent`, `red`, `currentColor`). Letters only. */
const NAMED_COLOR = /^[a-z]{3,20}$/i;

/** One family in a font stack, quotes already stripped. Letters, digits, spaces and the
 * two joiners real family names use -- nothing that could terminate a declaration. */
const FONT_FAMILY = /^[a-zA-Z0-9 _-]{1,64}$/;

/** Number's own toString switches to exponent notation past 1e21, which would put an `e`
 * and a `+` into a value the header promises is digits, a dot and a sign only. The bound
 * is orders of magnitude above any property's range and exists so that promise holds
 * whatever ranges the table above later carries. */
const MAX_MAGNITUDE = 1e6;

function fixed(n: number): string {
  return String(Math.round(clamp(n, -MAX_MAGNITUDE, MAX_MAGNITUDE) * 1000) / 1000);
}

function sanitizeColor(raw: unknown): string | null {
  if (typeof raw !== "string") {
    return null;
  }
  const s = raw.trim();
  return HEX_COLOR.test(s) || FUNC_COLOR.test(s) || NAMED_COLOR.test(s) ? s : null;
}

/** Rebuilt from its accepted families rather than passed through, so the output is the
 * allowlist by construction. A family with a space is re-quoted; one that fails the
 * allowlist is dropped, and a stack that loses every family yields nothing. */
function sanitizeFont(raw: unknown): string | null {
  if (typeof raw !== "string") {
    return null;
  }
  const families: string[] = [];
  for (const part of raw.split(",")) {
    const name = part.trim().replace(/^["']|["']$/g, "").trim();
    if (FONT_FAMILY.test(name)) {
      families.push(name.includes(" ") ? '"' + name + '"' : name);
    }
  }
  return families.length > 0 ? families.join(", ") : null;
}

/** The CSS value for one property, or null when the stored value cannot be trusted. */
function sanitize(prop: StyleProp, raw: unknown): string | null {
  switch (prop.kind) {
    case "length":
    case "ratio":
    case "percent": {
      if (typeof raw !== "number" && typeof raw !== "string") {
        return null;
      }
      // Number("") and Number("   ") are both 0, which would clamp to the property's
      // minimum and render as a real value -- so a blank never reaches the coercion.
      if (typeof raw === "string" && raw.trim() === "") {
        return null;
      }
      const n = Number(raw);
      if (!Number.isFinite(n)) {
        return null;
      }
      const v = clamp(n, prop.min, prop.max);
      if (prop.kind === "length") {
        return fixed(v) + prop.unit;
      }
      return fixed(prop.kind === "percent" ? v / 100 : v);
    }
    case "color":
      return sanitizeColor(raw);
    case "font":
      return sanitizeFont(raw);
    case "enum":
      return typeof raw === "string" && prop.options.some((o) => o.value === raw) ? raw : null;
  }
}

// --- compilation ---------------------------------------------------------------------

/** `style_<slot>` is the whole convention: the runtime finds a slot's styling by key, so
 * a widget declaring one needs no code anywhere. The slot name reaches a selector, so its
 * character set is deliberately narrower than a JSON key's. */
const SLOT_KEY = /^style_([A-Za-z0-9_-]{1,64})$/;

/** Whether a resolved part is a length of zero. Reads its own sanitized output, so the
 * leading number is all there is to parse; a keyword such as currentColor is not a length
 * and answers false. */
function isZeroLength(value: string | undefined): boolean {
  return value !== undefined && parseFloat(value) === 0;
}

/** One stored key's CSS value. A key naming no property answers null, so a stale gate or
 * composite part cannot reach the stylesheet as `undefined`. */
function valueFor(style: Record<string, unknown>, key: string): string | null {
  const prop = PROP_BY_KEY.get(key);
  return prop ? sanitize(prop, style[key]) : null;
}

/** The declarations for one slot's stored style object, or "" when it configures nothing
 * this build understands -- which is what keeps an untouched slot from emitting a rule. */
function declarationsFor(style: Record<string, unknown>): string {
  const out: string[] = [];

  for (const d of DERIVED) {
    if (d.gateAny.some((k) => valueFor(style, k) !== null)) {
      out.push(d.css + ":" + d.value);
    }
  }

  for (const prop of STYLE_PROPS) {
    if (prop.css === null) {
      continue;
    }
    const value = sanitize(prop, style[prop.key]);
    if (value !== null) {
      out.push(prop.css + ":" + value);
    }
  }

  for (const c of COMPOSITES) {
    if (valueFor(style, c.gate) === null) {
      continue;
    }
    const parts = new Map(c.parts.map((p) => [p.key, valueFor(style, p.key) ?? p.fallback]));
    const rescue = c.rescue;
    if (rescue && rescue.whenAllZero.every((k) => isZeroLength(parts.get(k)))) {
      parts.set(rescue.part, rescue.value);
    }
    out.push(c.css + ":" + c.parts.map((p) => parts.get(p.key)).join(" "));
  }

  return out.length > 0 ? out.join(";") + ";" : "";
}

/** The stylesheet for every slot a widget's resolved fields configure: one rule per slot,
 * none for a slot left alone. Selectors carry only the name the key matched, which the
 * pattern above has already restricted to characters that cannot close the selector. */
export function cssForSlots(fields: Record<string, unknown>): string {
  const rules: string[] = [];
  for (const [key, value] of Object.entries(fields ?? {})) {
    const match = SLOT_KEY.exec(key);
    if (!match || !isPlainObject(value)) {
      continue;
    }
    const body = declarationsFor(value);
    if (body) {
      rules.push('[data-ov-slot="' + match[1] + '"]{' + body + "}");
    }
  }
  return rules.join("\n");
}
