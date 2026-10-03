// The overlay template filler, on its own so tests can run the real one: runtime.ts
// carries page side effects and cannot be imported outside an overlay document.

const kTemplateToken = /\{(\w+)\}/g;
// The lookbehind starts a match only at the head of a run of spaces, keeping a long run linear.
const kEmptyTemplateToken = /(?<! ) *\{(\w+)\}/g;

/** One run of a filled template: literal text (`key` null) or the value of the variable
 * `key`. Text effects that target only the variables animate the runs with a key. */
export interface TemplatePart {
  text: string;
  key: string | null;
}

// Fill `{key}` tokens from `values`. A key absent from `values` is not a variable and stays
// verbatim. An empty value (null, undefined or "") leaves along with the spaces before it,
// so an absent value never leaves "sent  !" behind. The rest fill in one pass, so a value
// that itself contains "{name}" is shown as typed, never expanded.
export function fillTemplate(text: string, values: Record<string, unknown>): string {
  return fillTemplateParts(text, values)
    .map((p) => p.text)
    .join("");
}

/** fillTemplate, keeping which runs came from which variable. Joining the parts' text gives
 * exactly fillTemplate's result: the same empty-value removal, the same single pass, and the
 * same trim of the whole string. Empty runs are dropped. */
export function fillTemplateParts(text: string, values: Record<string, unknown>): TemplatePart[] {
  const valueOf = (key: string): string | null => {
    if (!Object.prototype.hasOwnProperty.call(values, key)) {
      return null;
    }
    const v = values[key];
    return v == null ? "" : String(v);
  };
  const stripped = String(text ?? "").replace(kEmptyTemplateToken, (m, key: string) => (valueOf(key) === "" ? "" : m));
  const parts: TemplatePart[] = [];
  let last = 0;
  for (const m of stripped.matchAll(kTemplateToken)) {
    const at = m.index ?? 0;
    if (at > last) {
      parts.push({ text: stripped.slice(last, at), key: null });
    }
    const value = valueOf(m[1]);
    parts.push(value === null ? { text: m[0], key: null } : { text: value, key: m[1] });
    last = at + m[0].length;
  }
  if (last < stripped.length) {
    parts.push({ text: stripped.slice(last), key: null });
  }
  return trimParts(parts);
}

// Trim the joined string's ends without joining it: leading whitespace comes off the first
// runs and trailing whitespace off the last, a run left empty is dropped.
function trimParts(parts: TemplatePart[]): TemplatePart[] {
  const out = parts.map((p) => ({ ...p }));
  while (out.length > 0) {
    out[0].text = out[0].text.trimStart();
    if (out[0].text) break;
    out.shift();
  }
  while (out.length > 0) {
    const tail = out[out.length - 1];
    tail.text = tail.text.trimEnd();
    if (tail.text) break;
    out.pop();
  }
  return out.filter((p) => p.text !== "");
}
