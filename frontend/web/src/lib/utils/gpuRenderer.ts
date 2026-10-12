// Which GPU the page actually renders on, for the host's GPU safe mode (gpu_safe_mode.hpp).
// A page that paints proves nothing about hardware: after its GPU process crashes, Chromium
// falls back to SwiftShader and keeps painting. The WebGL renderer string is what tells the
// two apart, so the main window reports it once, after its first frame.

import { obs } from "$lib/api/bridge";
import { Cat } from "$lib/utils/logCategories";
import { log } from "$lib/utils/log";
import { nextFrame } from "$lib/utils/presented";

/** The slice of `Document` the probe needs, so a test can hand it a fake. */
export interface CanvasFactory {
  createElement(tag: "canvas"): { getContext(kind: "webgl"): WebGLRenderingContext | null };
}

/**
 * WebGL's unmasked renderer string, or "" when there is no WebGL context or the context
 * will not name its renderer. "" reads as software on the host: hardware has to be shown,
 * not assumed.
 */
export function webglRenderer(doc: CanvasFactory = document): string {
  const gl = doc.createElement("canvas").getContext("webgl");
  if (!gl) {
    return "";
  }
  try {
    const info = gl.getExtension("WEBGL_debug_renderer_info");
    const name: unknown = info ? gl.getParameter(info.UNMASKED_RENDERER_WEBGL) : null;
    return typeof name === "string" ? name : "";
  } finally {
    gl.getExtension("WEBGL_lose_context")?.loseContext();
  }
}

let reported = false;

/** Report the renderer once, after the first frame. Main window only. */
export function reportGpuRendererAfterFirstPaint(): void {
  if (reported) {
    return;
  }
  reported = true;
  // Bounded frames: a window that starts hidden (minimized to tray) runs no rAF, and a
  // report that never comes reads as "no hardware" on the next boot.
  void nextFrame()
    .then(nextFrame)
    .then(() => obs.call("diagnostics.reportRenderer", { renderer: webglRenderer() }))
    .catch((e: unknown) => log.warn(Cat.lifecycle, "GPU renderer report failed", e));
}
