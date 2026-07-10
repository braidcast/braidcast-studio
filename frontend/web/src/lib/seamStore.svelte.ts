// Shared geometry for the nav-rail↔content "car-door" seam. NavRail measures the
// active item's vertical center (ym, relative to the rail/view top) and writes it
// here; App.svelte reads ym to author the content notch and the hairline liner.
// Both sides derive their clip-paths from the same constants and offset math so the
// gap traces a constant *perpendicular* width — straight, out around the point,
// straight again — instead of a horizontal-only shear.

// Frame: x=0 at the rail's straight right edge (abs 70px); +x points into content.
// TIP is a single knob so a flat/truncated point is a one-line change.
export const SEAM = {
  W: 70, // rail width / straight seam x — must equal .rail width in NavRail.svelte
  D: 18, // bump depth: the apex sits at x=D
  G: 5, // constant gap width between the rail edge and the content edge
  HALF: 16, // bump base half-height
  LINE: 1.5, // hairline width — the --color-border liner and the accent bump rim
  TIP: "sharp" as "sharp" | "flat",
  FLAT: 6, // flat-tip half-height (used only when TIP === "flat")
  EXTENT: 100000, // covers the viewport for the far/bottom clip vertices
} as const;

export type Pt = [number, number];

interface Line {
  px: number;
  py: number;
  dx: number;
  dy: number;
}

// Active item's vertical center, in the seam frame (0 = rail/view top). NavRail owns
// the write; App.svelte is the sole reader. Single source of truth for the seam Y.
export const seamStore = $state<{ ym: number }>({ ym: 0 });

// Right-side seam of the rail, top→bottom, in the seam frame (x=0 at the straight
// edge, apex at x=D). This is the shape the rail "adds" as a bump.
export function railSeam(ym: number): Pt[] {
  const { D, HALF, FLAT, TIP, EXTENT } = SEAM;
  const pts: Pt[] = [
    [0, 0],
    [0, ym - HALF],
  ];
  if (TIP === "flat") {
    pts.push([D, ym - FLAT], [D, ym + FLAT]);
  } else {
    pts.push([D, ym]);
  }
  pts.push([0, ym + HALF], [0, EXTENT]);
  return pts;
}

// The bump's gap-facing outline (the two diagonals), top→bottom, in the seam frame.
// Offsetting this by LINE yields the accent rim that traces the active triangle.
export function bumpDiagonals(ym: number): Pt[] {
  const { D, HALF, FLAT, TIP } = SEAM;
  const pts: Pt[] = [[0, ym - HALF]];
  if (TIP === "flat") {
    pts.push([D, ym - FLAT], [D, ym + FLAT]);
  } else {
    pts.push([D, ym]);
  }
  pts.push([0, ym + HALF]);
  return pts;
}

// Offset an open, top-to-bottom polyline to its right (+x, into content) by g,
// returning the parallel chain — a constant perpendicular distance g on every edge.
// This is why the gap stays even around the point and not just horizontally.
export function offsetRight(pts: Pt[], g: number): Pt[] {
  const lines: Line[] = [];
  for (let i = 0; i < pts.length - 1; i++) {
    const [ax, ay] = pts[i];
    const [bx, by] = pts[i + 1];
    const dx = bx - ax;
    const dy = by - ay;
    const len = Math.hypot(dx, dy) || 1;
    const nx = dy / len;
    const ny = -dx / len; // right normal for downward travel
    lines.push({ px: ax + nx * g, py: ay + ny * g, dx, dy });
  }
  const out: Pt[] = [[lines[0].px, lines[0].py]];
  for (let i = 1; i < lines.length; i++) {
    out.push(intersect(lines[i - 1], lines[i]));
  }
  const last = lines[lines.length - 1];
  out.push([last.px + last.dx, last.py + last.dy]);
  return out;
}

function intersect(a: Line, b: Line): Pt {
  const den = a.dx * b.dy - a.dy * b.dx;
  if (Math.abs(den) < 1e-6) {
    return [b.px, b.py];
  }
  const t = ((b.px - a.px) * b.dy - (b.py - a.py) * b.dx) / den;
  return [a.px + a.dx * t, a.py + a.dy * t];
}

export function polygon(pts: Pt[]): string {
  return "polygon(" + pts.map(([x, y]) => `${x.toFixed(2)}px ${y.toFixed(2)}px`).join(", ") + ")";
}
