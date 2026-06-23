import type { Component } from "svelte";
import type { AddPanelOptions } from "dockview-core";
import PlaceholderDock from "../docks/PlaceholderDock.svelte";
import ControlsDock from "../docks/ControlsDock.svelte";
import ScenesDock from "../docks/ScenesDock.svelte";
import SourcesDock from "../docks/SourcesDock.svelte";

// One entry per dock in the §3.5 inventory. `id` is the stable Dockview panel id
// (also the dock id the future window.detach uses). `accent` marks Controls /
// canvas / Multistream docks that render with the accent header in the mocks.
// `component` + `params` feed the mount adapter. Adding a dock is a single push.
export interface DockDef {
  id: string;
  title: string;
  component: Component<Record<string, unknown>>;
  params: Record<string, unknown>;
  accent?: boolean;
}

export const DOCKS: DockDef[] = [
  {
    id: "preview",
    title: "Preview · Default Canvas",
    component: PlaceholderDock,
    params: { label: "Program / Preview", phase: "P2" },
  },
  { id: "scenes", title: "Scenes", component: ScenesDock, params: {} },
  { id: "sources", title: "Sources", component: SourcesDock, params: {} },
  { id: "mixer", title: "Audio Mixer", component: PlaceholderDock, params: { label: "Faders", phase: "P4" } },
  { id: "transitions", title: "Transitions", component: PlaceholderDock, params: { label: "Fade", phase: "P4" } },
  { id: "controls", title: "Controls", component: ControlsDock, params: {}, accent: true },
  {
    id: "canvas-placeholder",
    title: "Canvas",
    component: PlaceholderDock,
    params: { label: "Canvas preview", phase: "P3" },
    accent: true,
  },
  {
    id: "multistream",
    title: "Multistream",
    component: PlaceholderDock,
    params: { label: "Destinations", phase: "P3" },
    accent: true,
  },
];

export function dockById(id: string): DockDef | undefined {
  return DOCKS.find((d) => d.id === id);
}

// Build the AddPanelOptions for a dock id. Title + accent flag + the detach seam
// ride in params so the custom tab can read them (keys prefixed __ are stripped
// before reaching the Svelte content body by the mount adapter). Standalone (not a
// DockHost method) so callers don't depend on a `bind:this` ref that Svelte 5 only
// assigns after the child mounts — onReady fires during that mount.
export function panelOptions(
  id: string,
  detachDock: (panelId: string) => void,
  extra: Partial<AddPanelOptions> = {},
): AddPanelOptions {
  const def = dockById(id);
  if (!def) {
    throw new Error(`panelOptions: unknown dock id "${id}"`);
  }
  return {
    id: def.id,
    component: def.id,
    title: def.title,
    params: { ...def.params, __accent: def.accent ?? false, __detach: detachDock },
    ...extra,
  };
}
