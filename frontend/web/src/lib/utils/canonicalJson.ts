// JSON with every object's keys in sorted order, at every depth: the form the host writes
// (nlohmann's default json keeps objects in a sorted map), so a document the page built
// locally and the same document read back from the host stringify identically. Arrays keep
// their order -- it is part of the value.
import { isPlainObject } from "./plainObject";

function sortDeep(v: unknown): unknown {
  if (Array.isArray(v)) {
    return v.map(sortDeep);
  }
  if (!isPlainObject(v)) {
    return v;
  }
  const out: Record<string, unknown> = {};
  for (const k of Object.keys(v).sort()) {
    out[k] = sortDeep(v[k]);
  }
  return out;
}

export function canonicalJson(v: unknown): string {
  return JSON.stringify(sortDeep(v));
}
