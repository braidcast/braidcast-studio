// A tiny LIFO of "Escape owners" so stacked overlays (a ContextMenu opened over a
// Modal, a submenu over its parent) don't all close on a single Escape press. Each
// layer pushes a token on mount and pops it on destroy; a layer's window/document
// Escape handler acts only when its token is topmost. Single-layer usage is
// unaffected (the sole token is always topmost).
//
// Layers carry a kind because "something is open" and "the app is behind a modal" are
// different facts, and an ambient surface has to tell them apart: a modal covers the
// studio's bottom bar and makes it unreachable, while a context menu leaves it live.
type EscKind = "modal" | "layer";

const stack: { token: symbol; kind: EscKind }[] = [];
const watchers = new Set<() => void>();

function notify(): void {
  for (const w of watchers) {
    w();
  }
}

export function pushEsc(kind: EscKind = "layer"): symbol {
  const token = Symbol();
  stack.push({ token, kind });
  notify();
  return token;
}

export function popEsc(token: symbol): void {
  const i = stack.findIndex((e) => e.token === token);
  if (i >= 0) {
    stack.splice(i, 1);
    notify();
  }
}

export function isTopEsc(token: symbol): boolean {
  return stack.length > 0 && stack[stack.length - 1].token === token;
}

/** True while any layer owns Escape. An ambient surface that the user never opened -- a toast
 *  pushes itself in front of whatever they were doing -- takes the key only when nothing
 *  layered is there to consume it, so it cannot become the topmost owner and shadow the dialog
 *  underneath it. Such a surface must NOT pushEsc(): mount order would put it on top. */
export function anyEscOwner(): boolean {
  return stack.length > 0;
}

/** True while a modal is open, which is narrower than anyEscOwner(): it means the studio behind
 *  it is covered and its controls cannot be clicked. A context menu does not count -- the bar
 *  underneath one is still live, so an overlay must not move on top of it. */
export function anyModalOpen(): boolean {
  return stack.some((e) => e.kind === "modal");
}

/** Observe layer changes. Returns an unsubscribe. Needed because this module is plain state, so
 *  a consumer that renders off anyModalOpen() has nothing to re-run on otherwise. */
export function watchEscStack(fn: () => void): () => void {
  watchers.add(fn);
  return () => {
    watchers.delete(fn);
  };
}
