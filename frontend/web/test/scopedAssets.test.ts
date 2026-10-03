import { describe, expect, test } from "bun:test";
import {
  assetFileOf,
  assetKindOf,
  assetPreviewUrl,
  formatBytes,
  scopedAssetKey,
  trackUpload,
  uploadInFlight,
  uploadProblem,
} from "../src/lib/overlays/scopes/scopedAssets";
import { canonicalJson } from "../src/lib/utils/canonicalJson";

const MB = 1024 * 1024;
const LIMITS = { sound: 8 * MB, image: 8 * MB, video: 32 * MB };

describe("scoped uploads", () => {
  test("the key carries the scope and the field, and keeps a safe extension", () => {
    expect(scopedAssetKey("default", "sound", "Ding.OGG")).toBe("default-sound.ogg");
    expect(scopedAssetKey("v_8f2c", "media", "party.webm")).toBe("v_8f2c-media.webm");
    expect(scopedAssetKey("cheer", "media", "no-extension")).toBe("cheer-media.bin");
    expect(scopedAssetKey("cheer", "media", "weird.p n g")).toBe("cheer-media.bin");
  });

  test("WebM is video, other images are images, audio is sound, the rest is refused", () => {
    expect(assetKindOf({ type: "video/webm", name: "a.webm" })).toBe("video");
    expect(assetKindOf({ type: "", name: "a.WEBM" })).toBe("video");
    expect(assetKindOf({ type: "image/apng", name: "a.png" })).toBe("image");
    expect(assetKindOf({ type: "audio/ogg", name: "a.ogg" })).toBe("sound");
    expect(assetKindOf({ type: "application/pdf", name: "a.pdf" })).toBeNull();
  });

  test("an oversize file is refused with the cap in the message", () => {
    expect(uploadProblem({ size: 8 * MB }, "image", LIMITS)).toBeNull();
    expect(uploadProblem({ size: 9 * MB }, "image", LIMITS)).toBe("Images can be up to 8 MB. This file is 9 MB.");
    expect(uploadProblem({ size: 31 * MB }, "video", LIMITS)).toBeNull();
    expect(uploadProblem({ size: 33 * MB }, "video", LIMITS)).toContain("up to 32 MB");
  });

  test("stored references and preview URLs", () => {
    expect(assetFileOf("assets/cheer-media.webm")).toBe("cheer-media.webm");
    expect(assetFileOf("library:coin-01")).toBeNull();
    expect(assetFileOf("")).toBeNull();
    expect(assetPreviewUrl("http://127.0.0.1:41234/w/abc?t=tok", "cheer-media.webm")).toBe(
      "http://127.0.0.1:41234/w/abc/assets/cheer-media.webm?t=tok",
    );
    expect(formatBytes(512)).toBe("1 KB");
    expect(formatBytes(3 * MB)).toBe("3 MB");
  });
});

describe("uploads in flight", () => {
  test("an upload counts from its start until it settles, failed or not", async () => {
    expect(uploadInFlight()).toBe(false);
    let finish!: () => void;
    const done = trackUpload(() => new Promise<void>((res) => (finish = res)));
    expect(uploadInFlight()).toBe(true);
    const failed = trackUpload(() => Promise.reject(new Error("refused")));
    await expect(failed).rejects.toThrow("refused");
    expect(uploadInFlight()).toBe(true);
    finish();
    await done;
    expect(uploadInFlight()).toBe(false);
  });
});

describe("canonical JSON", () => {
  test("keys sort at every depth; arrays keep their order", () => {
    expect(canonicalJson({ b: 1, a: { d: [2, 1], c: { f: 1, e: 2 } } })).toBe(
      '{"a":{"c":{"e":2,"f":1},"d":[2,1]},"b":1}',
    );
    expect(canonicalJson({ inAnim: { speed: 2, preset: "fade" } })).toBe(
      canonicalJson({ inAnim: { preset: "fade", speed: 2 } }),
    );
  });
});
