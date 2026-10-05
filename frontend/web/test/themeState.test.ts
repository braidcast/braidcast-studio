import { describe, expect, mock, test } from "bun:test";

mock.module("$lib/api/bridge", () => ({ obs: { call: async () => null, on: () => () => {} } }));
mock.module("$lib/theme/tokens", () => ({ applyTheme: () => {} }));
const { readThemeState } = await import("../src/lib/theme/themeStore.svelte");
const { DEFAULT_PRESET_ID, defaultTokens } = await import("../src/lib/theme/presets");

const custom = { id: "custom-mine-0", name: "Mine", tokens: { ...defaultTokens(), colorAccent: "#123456" } };

describe("reading a saved theme", () => {
  test("a whole state reads back as saved, with nothing lost", () => {
    const blob = JSON.stringify({ activeId: custom.id, activeTokens: custom.tokens, customThemes: [custom] });
    const read = readThemeState(blob);
    expect(read.lossy).toBe(false);
    expect(read.activeId).toBe(custom.id);
    expect(read.customThemes).toEqual([custom]);
    expect(read.tokens.colorAccent).toBe("#123456");
  });

  test("an old state missing tokens and customs is not lossy, and gains the defaults", () => {
    const read = readThemeState(JSON.stringify({ activeId: "x" }));
    expect(read.lossy).toBe(false);
    expect(read.customThemes).toEqual([]);
    expect(read.tokens).toEqual(defaultTokens());
  });

  test("an unparseable or non-object blob falls back to the default and says it lost something", () => {
    for (const blob of ["{not json", "null", "42", "[1,2]", '"text"']) {
      const read = readThemeState(blob);
      expect(read.lossy).toBe(true);
      expect(read.activeId).toBe(DEFAULT_PRESET_ID);
      expect(read.customThemes).toEqual([]);
    }
  });

  test("a custom theme entry that is not one is dropped, keeping the rest, and that is lossy", () => {
    const read = readThemeState(JSON.stringify({ activeId: custom.id, customThemes: [custom, { id: 3 }, null] }));
    expect(read.lossy).toBe(true);
    expect(read.customThemes).toEqual([custom]);
    expect(readThemeState(JSON.stringify({ customThemes: "oops" })).lossy).toBe(true);
  });
});
