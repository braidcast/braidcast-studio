// A tiny LIFO of "Escape owners" so stacked overlays (a ContextMenu opened over a
// Modal, a submenu over its parent) don't all close on a single Escape press. Each
// layer pushes a token on mount and pops it on destroy; a layer's window/document
// Escape handler acts only when its token is topmost. Single-layer usage is
// unaffected (the sole token is always topmost).
const stack: symbol[] = [];

export function pushEsc(): symbol {
  const token = Symbol();
  stack.push(token);
  return token;
}

export function popEsc(token: symbol): void {
  const i = stack.indexOf(token);
  if (i >= 0) {
    stack.splice(i, 1);
  }
}

export function isTopEsc(token: symbol): boolean {
  return stack.length > 0 && stack[stack.length - 1] === token;
}
