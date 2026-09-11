// Space is the preview's pan modifier, and the native overlay samples the key
// itself (it never holds the keyboard focus, so it cannot be sent WM_KEYDOWN and
// polls GetKeyState inside its mouse messages instead). That works, but it only
// covers the native half: the page still has focus, so the same keypress also
// reaches whatever is focused there, and SPACE on a focused button is a click. Hold
// space to pan after having clicked a toolbar button and the browser fires that
// button.
//
// So the web view has to suppress space's default action -- but only while the
// pointer is actually over a preview surface, or it would break space everywhere
// else. The DOM cannot work that out on its own: the overlay is a native HWND
// painted above the web view, so no pointer event ever fires for that region. The
// host tells us instead, edge-triggered, via preview.pointerOver.

import { obs } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
import { WINDOW_ID } from "$lib/utils/windowContext";

// Which surfaces in this window currently have the pointer. Keyed by canvas uuid
// ("" for the Default surface) rather than a bare counter: an enter for one surface
// and a leave for another can interleave while the pointer crosses a dock edge, and
// a counter would drift out of step where a set cannot.
const over = new Set<string>();

let refs = 0;
let teardown: (() => void) | null = null;

function onKeyDown(e: KeyboardEvent): void {
  if (e.code !== "Space" || over.size === 0) {
    return;
  }
  // A text field with the pointer parked over the preview is still a text field --
  // typing a space there must insert one. Only the activation default is the
  // problem, so leave editable targets alone.
  const t = e.target as HTMLElement | null;
  if (t?.isContentEditable || t instanceof HTMLInputElement || t instanceof HTMLTextAreaElement) {
    return;
  }
  e.preventDefault();
}

// Ref-counted so the two dock types can each ask for it without installing two
// listeners, and so it comes off when the last preview-hosting dock unmounts.
export function usePreviewPointerGuard(): () => void {
  if (refs === 0) {
    const offOver = obs.on(EV.previewPointerOver, (p) => {
      if (p.window !== WINDOW_ID) {
        return;
      }
      const key = p.canvas ?? "";
      if (p.over) {
        over.add(key);
      } else {
        over.delete(key);
      }
    });
    // Capture phase: the focused control's own handler would otherwise see the
    // event first and act on it before this could stop the default.
    window.addEventListener("keydown", onKeyDown, true);
    teardown = () => {
      offOver();
      window.removeEventListener("keydown", onKeyDown, true);
      over.clear();
    };
  }
  refs++;

  let released = false;
  return () => {
    if (released) {
      return;
    }
    released = true;
    refs--;
    if (refs === 0 && teardown) {
      teardown();
      teardown = null;
    }
  };
}
