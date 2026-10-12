import { describe, expect, mock, test } from "bun:test";

// The module reports through the bridge, which wires window globals on import.
mock.module("$lib/api/bridge", () => ({ obs: { call: async () => ({ ok: true }), on: () => () => {} } }));

const { webglRenderer } = await import("../src/lib/utils/gpuRenderer");

const RENDERER_PARAM = 0x9246;

function fakeDoc(gl: unknown) {
  return { createElement: () => ({ getContext: () => gl as WebGLRenderingContext | null }) };
}

function fakeGl(opts: { renderer?: unknown; debugInfo?: boolean }) {
  const lost = { count: 0 };
  const gl = {
    getExtension(name: string) {
      if (name === "WEBGL_debug_renderer_info") {
        return opts.debugInfo === false ? null : { UNMASKED_RENDERER_WEBGL: RENDERER_PARAM };
      }
      if (name === "WEBGL_lose_context") {
        return { loseContext: () => lost.count++ };
      }
      return null;
    },
    getParameter(p: number) {
      return p === RENDERER_PARAM ? opts.renderer : "WebKit WebGL";
    },
  };
  return { gl, lost };
}

describe("webglRenderer", () => {
  test("names the hardware renderer and releases the context", () => {
    const name = "ANGLE (NVIDIA, NVIDIA GeForce RTX 5070 Ti (0x00002C05) Direct3D11 vs_5_0 ps_5_0, D3D11)";
    const { gl, lost } = fakeGl({ renderer: name });
    expect(webglRenderer(fakeDoc(gl))).toBe(name);
    expect(lost.count).toBe(1);
  });

  test("is empty without a WebGL context", () => {
    expect(webglRenderer(fakeDoc(null))).toBe("");
  });

  test("is empty when the context will not name its renderer", () => {
    const { gl, lost } = fakeGl({ debugInfo: false });
    expect(webglRenderer(fakeDoc(gl))).toBe("");
    expect(lost.count).toBe(1);
  });
});
