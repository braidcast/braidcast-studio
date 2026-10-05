import { describe, expect, mock, test } from "bun:test";

const calls: { method: string; params?: unknown }[] = [];
const listeners: Record<string, (p: unknown) => void> = {};
let status: unknown = null;
mock.module("$lib/api/bridge", () => ({
  obs: {
    call: async (method: string, params?: unknown) => {
      calls.push({ method, params });
      return method === "update.status" ? status : { ok: true };
    },
    on: (ev: string, fn: (p: unknown) => void) => {
      listeners[ev] = fn;
      return () => {};
    },
  },
}));
const toasts: { message: string; opts: { action?: { label: string; onAction: () => void } } }[] = [];
mock.module("$lib/stores/toastStore.svelte", () => ({
  showToast: (message: string, _title: string, opts: never) => void toasts.push({ message, opts }),
}));
const { noticeText, startUpdateNotice } = await import("../src/lib/stores/updateNotice");

describe("the launch update notice", () => {
  test("reads the version without its v, and names where to get it for a screen reader", () => {
    expect(noticeText({ version: "v0.10.0", url: "https://braidcast.com/download" })).toEqual({
      message: "Braidcast 0.10.0 is available.",
      announce: "Braidcast 0.10.0 is available. Download it from braidcast.com.",
    });
  });

  test("a notice found before the page loaded shows once, is acked, and the Download button opens the page", async () => {
    status = { version: "v0.10.0", url: "https://braidcast.com/download" };
    startUpdateNotice();
    await new Promise((r) => setTimeout(r, 0));
    expect(toasts.map((t) => t.message)).toEqual(["Braidcast 0.10.0 is available."]);
    expect(calls.filter((c) => c.method === "update.ack")).toEqual([{ method: "update.ack", params: { version: "v0.10.0" } }]);
    toasts[0].opts.action?.onAction();
    expect(calls.at(-1)).toEqual({ method: "shell.openUrl", params: { url: "https://braidcast.com/download" } });
  });

  test("the same version arriving again by event is not shown twice; a newer one is", () => {
    listeners["update.available"]({ version: "v0.10.0", url: "https://braidcast.com/download" });
    expect(toasts).toHaveLength(1);
    listeners["update.available"]({ version: "v0.11.0", url: "https://braidcast.com/download" });
    expect(toasts.map((t) => t.message)).toEqual(["Braidcast 0.10.0 is available.", "Braidcast 0.11.0 is available."]);
  });
});
