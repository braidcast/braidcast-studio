import { describe, expect, test } from "bun:test";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { compile } from "svelte/compiler";
import { render } from "svelte/server";
import { frozenFrameFrom, placeStill, sameSurfaceSize, type FrozenFrame } from "$lib/docking/freezeFrame";
import type { OverlayRect } from "$lib/utils/overlayRect";

const px = (v: string) => {
  expect(v.endsWith("px")).toBe(true);
  return parseFloat(v);
};

// Issue #27's session: the Default surface at 388x689 CSS px and dpr 1.
const ELEMENT: OverlayRect = { x: 766, y: 125, w: 388, h: 689, dpr: 1 };

describe("placeStill", () => {
  test("covers the element at one image pixel per device pixel", () => {
    const p = placeStill(388, 689, ELEMENT)!;
    expect([px(p.left), px(p.top), px(p.width), px(p.height)]).toEqual([0, 0, 388, 689]);
  });

  test("starts on the device pixel the host rounds the surface to", () => {
    // At dpr 1.25 the element's left edge, 766.3 CSS px, is device px 957.875; the host puts
    // the HWND at 958, so the still moves right by the 0.125 device px it rounded up.
    const element: OverlayRect = { x: 766.3, y: 125.1, w: 388, h: 689, dpr: 1.25 };
    const p = placeStill(485, 861, element)!;
    expect((element.x + px(p.left)) * 1.25).toBeCloseTo(958, 9);
    expect((element.y + px(p.top)) * 1.25).toBeCloseTo(156, 9);
    expect(px(p.width) * 1.25).toBeCloseTo(485, 9);
    expect(px(p.height) * 1.25).toBeCloseTo(861, 9);
  });

  test("refuses a still drawn for another surface size", () => {
    // The element has since grown; a still for the old size is a different picture.
    expect(placeStill(388, 689, { ...ELEMENT, w: 500 })).toBeNull();
    expect(placeStill(388, 689, { ...ELEMENT, dpr: 1.25 })).toBeNull();
    // Rounding to device px is the host's; within a pixel is the same surface.
    expect(placeStill(388, 689, { ...ELEMENT, w: 388.4 })).not.toBeNull();
  });

  test("refuses an empty still", () => {
    expect(placeStill(0, 689, ELEMENT)).toBeNull();
  });
});

describe("frozenFrameFrom", () => {
  test("a reply the size of the surface becomes the still", () => {
    const f = frozenFrameFrom({ dataUri: "data:x", width: 388, height: 689 }, ELEMENT)!;
    expect(f.dataUri).toBe("data:x");
    expect(f.element).toEqual(ELEMENT);
    expect(px(f.placement.width)).toBe(388);
  });

  test("a reply for another size is a failed capture", () => {
    expect(frozenFrameFrom({ dataUri: "data:x", width: 720, height: 405 }, ELEMENT)).toBeNull();
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
        { dataUri: "data:x", width: 388, height: 689 },
        ELEMENT,
      )!;
      body = render(Still, { props: { freeze: { frame, img: undefined, paintId: "freeze-7" } } }).body;
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
    expect(body).toContain('src="data:x"');
    // What capture() waits on to know the still has been presented.
    expect(body).toContain('elementtiming="freeze-7"');
    // Asks for the decode to finish before the frame that paints it, so the frame the entry
    // reports is meant to carry its pixels rather than a placeholder.
    expect(body).toContain('decoding="sync"');
    expect(body).toMatch(/left:\s*0px/);
    expect(body).toMatch(/top:\s*0px/);
    expect(body).toMatch(/width:\s*388px/);
    expect(body).toMatch(/height:\s*689px/);
    expect(out.css?.code ?? "").not.toMatch(/object-fit:\s*(contain|cover)/);
  });
});
