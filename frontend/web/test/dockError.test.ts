import { describe, expect, mock, test } from "bun:test";

const toasts: { message: string; title: string; opts: Record<string, unknown> }[] = [];
const toast: { current: { message: string; kind: unknown } | null } = { current: null };
mock.module("$lib/stores/toastStore.svelte", () => ({
  showToast: (message: string, title: string, opts: Record<string, unknown>) => {
    toasts.push({ message, title, opts });
    toast.current = { message, kind: opts.kind };
  },
  toast,
}));
const { DockError, DOCK_ERROR_TOAST_KIND, DOCK_ERROR_TOAST_MS } = await import("../src/lib/docking/dockError.svelte");

describe("a dock's error reporting", () => {
  test("a failed action is a dismissible, assertive toast and never the in-dock banner", () => {
    toasts.length = 0;
    toast.current = null;
    const dockError = new DockError();
    dockError.report(new Error("a source named 'Game Capture' already exists"));
    expect(toasts).toEqual([
      {
        message: "a source named 'Game Capture' already exists",
        title: "a source named 'Game Capture' already exists",
        opts: { assertive: true, dismissible: true, durationMs: DOCK_ERROR_TOAST_MS, kind: DOCK_ERROR_TOAST_KIND },
      },
    ]);
    expect(dockError.message).toBeNull();
  });

  test("report stays bound when handed straight to .catch", async () => {
    toasts.length = 0;
    toast.current = null;
    const { report } = new DockError();
    await Promise.reject(new Error("rename failed")).catch(report);
    expect(toasts.map((t) => t.message)).toEqual(["rename failed"]);
  });

  test("a failed load is the banner, without a toast, until the next successful load clears it", async () => {
    toasts.length = 0;
    toast.current = null;
    const dockError = new DockError();
    await Promise.reject(new Error("scenes.list failed")).catch(dockError.reportLoad);
    expect(dockError.message).toBe("scenes.list failed");
    expect(toasts).toEqual([]);
    dockError.report(new Error("an unrelated action failed"));
    expect(dockError.message).toBe("scenes.list failed");
    dockError.clear();
    expect(dockError.message).toBeNull();
  });
  test("the same failure repeated while its toast is up is not re-shown", () => {
    toasts.length = 0;
    toast.current = null;
    const { report } = new DockError();
    report(new Error("fader move failed"));
    report(new Error("fader move failed"));
    report(new Error("a different failure"));
    expect(toasts.map((t) => t.message)).toEqual(["fader move failed", "a different failure"]);
  });
});
