// Turns third-party markup into markup this app is willing to put in its own DOM.
// The Qt properties view fed OBS_TEXT_INFO values to a QLabel, whose rich-text engine
// is a small fixed subset; CEF is a whole browser and the string comes from plugin
// code, so the subset has to be re-imposed here.
//
// Two properties do the work. Parsing happens in a DOMParser document, which has no
// browsing context: nothing there executes and nothing there fetches. The result is
// then REBUILT node by node into freshly created elements, so an attribute reaches the
// output only by being copied across explicitly -- there is no path by which an
// unlisted one survives.

// Inline formatting plus links: what this field is used for. Anything structural,
// anything that loads a resource, and anything that can execute is absent by design.
const ALLOWED_TAGS = new Set(["a", "b", "strong", "i", "em", "u", "code", "br"]);

// Elements whose content is source rather than prose. Everything unlisted is UNWRAPPED
// (element dropped, text kept), which for these would print their own markup as visible
// text, so they are dropped whole instead. svg and math are here for a second reason:
// foreign-content namespaces are where parse/serialize round trips stop being faithful.
const DROPPED_SUBTREES = new Set([
  "script",
  "style",
  "template",
  "noscript",
  "iframe",
  "object",
  "embed",
  "title",
  "textarea",
  "xmp",
  "svg",
  "math",
]);

const SAFE_SCHEMES = new Set(["http:", "https:"]);

// Past this, a subtree is flattened to its text instead of recursed into, so hostile
// nesting cannot exhaust the stack. Real info text nests two or three deep.
const MAX_DEPTH = 16;

const ESCAPES: Record<string, string> = { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" };

function escapeText(raw: string): string {
  return raw.replace(/[&<>"]/g, (c) => ESCAPES[c]);
}

/** The URL an anchor may point at, normalized, or null if it may not point anywhere.
 * Parsed rather than pattern-matched because the URL parser is what strips the
 * whitespace and control characters that hide a `javascript:` behind a `java\tscript:`.
 * A relative href fails to parse and is rejected on purpose: relative to app://app/ it
 * would navigate the shell. */
function safeHref(raw: string): string | null {
  try {
    const url = new URL(raw);
    return SAFE_SCHEMES.has(url.protocol) ? url.href : null;
  } catch {
    return null;
  }
}

function rebuild(source: Node, target: Node, doc: Document, depth: number): void {
  for (const node of Array.from(source.childNodes)) {
    if (node.nodeType === Node.TEXT_NODE) {
      target.appendChild(doc.createTextNode(node.nodeValue ?? ""));
      continue;
    }
    if (node.nodeType !== Node.ELEMENT_NODE) {
      continue;
    }
    const el = node as Element;
    // localName rather than tagName: a namespaced element normalizes to a bare
    // lowercase name here, so no spelling of a tag slips past the two sets.
    const tag = el.localName;
    if (DROPPED_SUBTREES.has(tag)) {
      continue;
    }
    if (depth >= MAX_DEPTH) {
      target.appendChild(doc.createTextNode(el.textContent ?? ""));
      continue;
    }
    if (!ALLOWED_TAGS.has(tag)) {
      rebuild(el, target, doc, depth + 1);
      continue;
    }

    const copy = doc.createElement(tag);
    if (tag === "a") {
      const href = safeHref(el.getAttribute("href") ?? "");
      if (href === null) {
        rebuild(el, target, doc, depth + 1);
        continue;
      }
      copy.setAttribute("href", href);
      copy.setAttribute("title", href);
      // The click is intercepted and handed to the system browser. target=_blank is
      // what happens when that interception does not run: the host cancels every page
      // popup (Client::OnBeforePopup), so the fallback is a link that does nothing
      // rather than the main frame leaving app://app/ with no route back.
      copy.setAttribute("target", "_blank");
      copy.setAttribute("rel", "noopener noreferrer");
    }
    target.appendChild(copy);
    rebuild(el, copy, doc, depth + 1);
  }
}

function sanitizeOnce(raw: string): string {
  const doc = new DOMParser().parseFromString(raw, "text/html");
  const out = doc.createElement("div");
  rebuild(doc.body, out, doc, 0);
  return out.innerHTML;
}

/** Plugin-supplied markup reduced to inline formatting and http(s) links, as an HTML
 * string safe to insert. Everything else survives as its text. */
export function sanitizeInlineHtml(raw: string): string {
  const once = sanitizeOnce(raw);
  // Sanitizing an already-sanitized string has to be a no-op. A difference means the
  // serialize/re-parse round trip changed what the markup means -- the shape mutation
  // XSS takes -- so nothing from this input is trusted as markup.
  return sanitizeOnce(once) === once ? once : escapeText(raw);
}
