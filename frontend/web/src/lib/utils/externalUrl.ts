// The one way out of the app. CEF's main frame IS the UI, served from app://app/, so
// letting it navigate to an external URL replaces the whole shell with no route back.
// Every outbound link goes to the host instead, which hands the URL to the system
// browser (the same ShellExecute path the OAuth broker already opens its consent page
// with) and rejects any scheme that is not http(s).

import { obs } from "$lib/api/bridge";
import { log } from "$lib/utils/log";
import { Cat } from "$lib/utils/logCategories";

export function openExternalUrl(url: string): void {
  void obs.call("shell.openUrl", { url }).catch((e) => {
    log.warn(Cat.bridge, "shell.openUrl failed", url, e);
  });
}

/** Svelte action: route clicks on any link inside `node` to the system browser.
 * Delegated because the links come from sanitized markup rendered with {@html}, where
 * there is no element to hang a handler on. */
export function externalLinks(node: HTMLElement) {
  const onClick = (e: MouseEvent) => {
    const link = (e.target as Element | null)?.closest("a[href]");
    if (!link || !node.contains(link)) {
      return;
    }
    e.preventDefault();
    openExternalUrl(link.getAttribute("href") ?? "");
  };
  node.addEventListener("click", onClick);
  return {
    destroy: () => node.removeEventListener("click", onClick),
  };
}
