// The launch update check's notice (roadmap 10.6): the host holds a newer release it found
// until the page says it was shown (update.ack), so it shows once per version however the
// page and the check race -- the page asks at load, and the check's update.available covers
// a check that finishes later. Never nags: one toast, no badge, nothing repeated.

import { obs, type UpdateNotice } from "$lib/api/bridge";
import { showToast } from "$lib/stores/toastStore.svelte";
import { EV } from "$lib/utils/eventNames";

// Long enough to read and reach the button; the toast says what to do without it too.
const NOTICE_MS = 15000;

let started = false;
let shown = "";

/** The toast's words, so they are tested rather than read off the screen. */
export function noticeText(n: UpdateNotice): { message: string; announce: string } {
  const version = n.version.replace(/^v/i, "");
  return {
    message: `Braidcast ${version} is available.`,
    announce: `Braidcast ${version} is available. Download it from braidcast.com.`,
  };
}

function show(n: UpdateNotice | null): void {
  if (!n || !n.version || n.version === shown) {
    return;
  }
  shown = n.version;
  const { message, announce } = noticeText(n);
  showToast(message, "", {
    announce,
    dismissible: true,
    durationMs: NOTICE_MS,
    kind: "update",
    action: { label: "Download", onAction: () => void obs.call("shell.openUrl", { url: n.url }).catch(() => {}) },
  });
  void obs.call("update.ack", { version: n.version }).catch(() => {});
}

export function startUpdateNotice(): void {
  if (started) {
    return;
  }
  started = true;
  obs.on(EV.updateAvailable, show);
  void obs
    .call("update.status")
    .then(show)
    .catch(() => {});
}
