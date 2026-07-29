// Clamp `v` into [lo, hi]. An inverted range (hi < lo) collapses to `lo` rather
// than throwing, so a caller computing `hi` from a measured element that has not
// been laid out yet still gets a usable number.
export function clamp(v: number, lo: number, hi: number): number {
  return Math.min(Math.max(v, lo), Math.max(lo, hi));
}
