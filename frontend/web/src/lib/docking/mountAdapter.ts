import { mount, unmount, type Component } from "svelte";
import type { IContentRenderer, GroupPanelPartInitParameters } from "dockview-core";

// Bridges dockview-core's imperative panel API to Svelte 5 components: Dockview
// hands us a DOM element; we mount() a component into it on init and unmount() on
// dispose. params.params carries our per-panel props (see dockRegistry). The
// internal __* keys (accent flag, detach seam) are stripped before they reach the
// Svelte body so a dock component only sees its own declared props.
export function createContentRenderer(component: Component<Record<string, unknown>>): () => IContentRenderer {
  return () => {
    const element = document.createElement("div");
    element.style.height = "100%";
    element.style.width = "100%";
    element.style.overflow = "auto";
    let instance: Record<string, unknown> | null = null;

    return {
      element,
      init(params: GroupPanelPartInitParameters) {
        const props: Record<string, unknown> = {};
        for (const [key, value] of Object.entries(params.params ?? {})) {
          if (!key.startsWith("__")) {
            props[key] = value;
          }
        }
        instance = mount(component, { target: element, props });
      },
      dispose() {
        if (instance) {
          void unmount(instance);
          instance = null;
        }
      },
    };
  };
}
