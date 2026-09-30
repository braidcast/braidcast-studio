// The overlay template filler, on its own so tests can run the real one: runtime.ts
// carries page side effects and cannot be imported outside an overlay document.

const kTemplateToken = /\{(\w+)\}/g;
// The lookbehind starts a match only at the head of a run of spaces, keeping a long run linear.
const kEmptyTemplateToken = /(?<! ) *\{(\w+)\}/g;

// Fill `{key}` tokens from `values`. A key absent from `values` is not a variable and stays
// verbatim. An empty value (null, undefined or "") leaves along with the spaces before it,
// so an absent value never leaves "sent  !" behind. The rest fill in one pass, so a value
// that itself contains "{name}" is shown as typed, never expanded.
export function fillTemplate(text: string, values: Record<string, unknown>): string {
  const valueOf = (key: string): string | null => {
    if (!Object.prototype.hasOwnProperty.call(values, key)) {
      return null;
    }
    const v = values[key];
    return v == null ? "" : String(v);
  };
  return String(text ?? "")
    .replace(kEmptyTemplateToken, (m, key: string) => (valueOf(key) === "" ? "" : m))
    .replace(kTemplateToken, (m, key: string) => valueOf(key) ?? m)
    .trim();
}
