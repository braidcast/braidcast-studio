import { describe, expect, test } from "bun:test";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { compile } from "svelte/compiler";
import { render } from "svelte/server";
import type { PreviewCanvasRect } from "$lib/api/bridge";
import { frozenFrameFrom, placeCanvasRect, sameSurfaceSize, type FrozenFrame } from "$lib/docking/freezeFrame";
import type { OverlayRect } from "$lib/utils/overlayRect";

const px = (v: string) => {
  expect(v.endsWith("px")).toBe(true);
  return parseFloat(v);
};

// Issue #27's session: the Default surface at 388x689 CSS px and dpr 1, showing a
// 1440x2560 canvas fitted with the 10 px edge margin (preview_window.cpp PlaceCanvas).
const ELEMENT: OverlayRect = { x: 766, y: 125, w: 388, h: 689, dpr: 1 };
const FITTED: PreviewCanvasRect = { x: 10, y: 17, w: 368, h: 654, surfaceW: 388, surfaceH: 689 };

describe("placeCanvasRect", () => {
  test("puts the still on the canvas rect the host laid out, not the whole element", () => {
    const p = placeCanvasRect(FITTED, ELEMENT)!;
    expect([px(p.left), px(p.top), px(p.width), px(p.height)]).toEqual([10, 17, 368, 654]);
  });

  test("converts device px to CSS px at a fractional devicePixelRatio", () => {
    // The same element at dpr 1.5: the host sized the surface 582x1034 and fitted the
    // canvas at 562x999, 10 px in and centered vertically.
    const element = { ...ELEMENT, dpr: 1.5 };
    const p = placeCanvasRect({ x: 10, y: 17, w: 562, h: 999, surfaceW: 582, surfaceH: 1034 }, element)!;
    expect(px(p.left)).toBeCloseTo(10 / 1.5, 6);
    expect(px(p.top)).toBeCloseTo(17 / 1.5, 6);
    expect(px(p.width)).toBeCloseTo(562 / 1.5, 6);
    expect(px(p.height)).toBeCloseTo(999 / 1.5, 6);
  });

  test("keeps a zoomed and panned canvas that overhangs the element", () => {
    const element: OverlayRect = { x: 0, y: 0, w: 800, h: 450, dpr: 1 };
    const p = placeCanvasRect({ x: -300, y: -120, w: 1920, h: 1080, surfaceW: 800, surfaceH: 450 }, element)!;
    expect([px(p.left), px(p.top), px(p.width), px(p.height)]).toEqual([-300, -120, 1920, 1080]);
  });

  test("refuses a rect laid out for another surface size", () => {
    // The element has since grown; a rect for the old size would show the wrong picture.
    expect(placeCanvasRect(FITTED, { ...ELEMENT, w: 500 })).toBeNull();
    expect(placeCanvasRect(FITTED, { ...ELEMENT, dpr: 1.25 })).toBeNull();
    // Rounding to device px is the host's; within a pixel is the same surface.
    expect(placeCanvasRect(FITTED, { ...ELEMENT, w: 388.4 })).not.toBeNull();
  });

  test("refuses a missing or degenerate rect", () => {
    expect(placeCanvasRect(undefined, ELEMENT)).toBeNull();
    expect(placeCanvasRect({ ...FITTED, w: 0 }, ELEMENT)).toBeNull();
  });
});

describe("frozenFrameFrom", () => {
  test("a reply with a placeable rect becomes the still", () => {
    const f = frozenFrameFrom({ dataUri: "data:x", width: 1440, height: 2560, canvasRect: FITTED }, ELEMENT)!;
    expect(f.dataUri).toBe("data:x");
    expect(f.element).toEqual(ELEMENT);
    expect(px(f.placement.left)).toBe(10);
  });

  test("a reply without a rect is a failed capture", () => {
    expect(frozenFrameFrom({ dataUri: "data:x", width: 1440, height: 2560 }, ELEMENT)).toBeNull();
  });
});

test("sameSurfaceSize ignores position", () => {
  expect(sameSurfaceSize(ELEMENT, { ...ELEMENT, x: 0, y: 0 })).toBe(true);
  expect(sameSurfaceSize(ELEMENT, { ...ELEMENT, h: 690 })).toBe(false);
});

// The component, rendered on the server so bun needs no Svelte preload: the still must be
// drawn at the frame's placement, stretched into it, not fitted to the element.
describe("PreviewFreezeStill", () => {
  test("draws the held frame at its placement and stretches it there", async () => {
    const source = readFileSync(join(import.meta.dir, "../src/lib/docking/PreviewFreezeStill.svelte"), "utf-8");
    const out = compile(source, { generate: "server", filename: "PreviewFreezeStill.svelte" });
    // Resolve bare imports from this project, since the compiled module lives in a temp dir.
    const code = out.js.code.replace(/from (["'])([^"'.][^"']*)\1/g, (_m, _q, spec: string) => {
      const resolved = spec.startsWith("$lib/")
        ? join(import.meta.dir, "../src/lib", spec.slice(5)) + ".ts"
        : Bun.resolveSync(spec, import.meta.dir);
      return `from ${JSON.stringify(resolved)}`;
    });
    const dir = mkdtempSync(join(tmpdir(), "freeze-still-"));
    let body: string;
    try {
      const file = join(dir, "PreviewFreezeStill.js");
      writeFileSync(file, code);
      const { default: Still } = await import(file);
      const frame: FrozenFrame = frozenFrameFrom(
        { dataUri: "data:x", width: 1, height: 1, canvasRect: FITTED },
        ELEMENT,
      )!;
      body = render(Still, { props: { freeze: { frame, img: undefined } } }).body;
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
    expect(body).toContain('src="data:x"');
    expect(body).toMatch(/left:\s*10px/);
    expect(body).toMatch(/top:\s*17px/);
    expect(body).toMatch(/width:\s*368px/);
    expect(body).toMatch(/height:\s*654px/);
    expect(out.css?.code ?? "").not.toMatch(/object-fit:\s*(contain|cover)/);
  });
});
