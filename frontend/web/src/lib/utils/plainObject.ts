// Whether `v` is a JSON object rather than a primitive, an array, or null. Values reach
// both the fields panel and the overlay runtime as parsed JSON of unknown shape, so the
// two places that branch on "is this a nested settings object" ask the same question here.
export function isPlainObject(v: unknown): v is Record<string, unknown> {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}
