import type { Component } from "svelte";
import type { AddPanelOptions } from "dockview-core";
import PreviewDock from "$lib/docks/PreviewDock.svelte";
import ScenesDock from "$lib/docks/ScenesDock.svelte";
import SourcesDock from "$lib/docks/SourcesDock.svelte";
import MultistreamDock from "$lib/docks/MultistreamDock.svelte";
import MultichatDock from "$lib/docks/MultichatDock.svelte";
import EventsDock from "$lib/docks/EventsDock.svelte";
import ChannelsDock from "$lib/docks/ChannelsDock.svelte";
import AudioMixerDock from "$lib/docks/AudioMixerDock.svelte";
import TransitionsDock from "$lib/docks/TransitionsDock.svelte";
import StatsDock from "$lib/docks/StatsDock.svelte";

// One entry per dock in the §3.5 inventory. `id` is the stable Dockview panel id
// (also the dock id the future window.detach uses). `accent` marks the canvas /
// Multistream docks that render with the accent header in the mocks.
// `component` + `params` feed the mount adapter. Adding a dock is a single push.
export interface DockDef {
  id: string;
  title: string;
  component: Component<Record<string, unknown>>;
  params: Record<string, unknown>;
  accent?: boolean;
}

// Default width (px) for a dock added programmatically into a horizontal row
// (restore chip, redock, canvas/browser reconciler). Passing an explicit size
// makes Dockview's splitview take THIS width from its neighbors instead of
// falling back to Sizing.Distribute, which equalizes every sibling column and
// wipes a user's manually-narrowed dock. Also the single knob for "a bit
// narrower by default" — tune here, not per call site.
export const SIDE_DOCK_WIDTH = 320;

export const DOCKS: DockDef[] = [
  // Default canvas preview. The tab carries a status dot + a `GLOBAL S/S` badge
  // (mock dockHead): the Default canvas renders the global scenes/sources, so its
  // header is labelled distinctly from the additional canvases' `OWN S/S` docks.
  // __-prefixed keys feed the custom tab and are stripped before the Svelte body.
  {
    id: "preview",
    title: "Preview · Main",
    component: PreviewDock,
    params: { __dot: "var(--color-muted)", __badge: "GLOBAL S/S" },
  },
  { id: "scenes", title: "Scenes", component: ScenesDock, params: {} },
  { id: "sources", title: "Sources", component: SourcesDock, params: {} },
  { id: "mixer", title: "Audio Mixer", component: AudioMixerDock, params: {} },
  { id: "transitions", title: "Transitions", component: TransitionsDock, params: {} },
  { id: "multistream", title: "Multistream", component: MultistreamDock, params: {}, accent: true },
  // Merged read+send chat across every connected platform (Phase 9.0). Like
  // Multistream/Transitions it is NOT in the default layout -- it opens from the
  // CANVASES-bar restore chip -- but stays registered so it is addable/restorable.
  { id: "multichat", title: "Multichat", component: MultichatDock, params: {}, accent: true },
  // Live cross-platform events feed (follows/subs/gifts/cheers/raids/superchats,
  // Phase 9.2). Like Multichat it is NOT in the default layout -- it opens from the
  // CANVASES-bar restore chip -- but stays registered so it is addable/restorable.
  { id: "events", title: "Events", component: EventsDock, params: {}, accent: true },
  // Per-account identity + audience (Channel-identity feature). Like Events/Multichat
  // it is meaningless without a logged-in account, so it is NOT in the default layout
  // and is OAuth-gated -- it opens from the CANVASES-bar restore chip once an account
  // is connected, but stays registered so it is addable/restorable.
  { id: "channels", title: "Channels", component: ChannelsDock, params: {} },
  { id: "stats", title: "Stats", component: StatsDock, params: {} },
];

export function dockById(id: string): DockDef | undefined {
  return DOCKS.find((d) => d.id === id);
}

// Build the AddPanelOptions for a dock id. Title + accent flag ride in params so
// the custom tab can read them (keys prefixed __ are stripped before reaching the
// Svelte content body by the mount adapter). The tear-out handler is NOT threaded
// through params — it is resolved at click time from detachRegistry, so a panel
// rebuilt from a saved layout (which JSON.stringify strips functions from) still
// detaches. Standalone (not a DockHost method) so callers don't depend on a
// `bind:this` ref that Svelte 5 only assigns after the child mounts — onReady fires
// during that mount.
export function panelOptions(id: string, extra: Partial<AddPanelOptions> = {}): AddPanelOptions {
  const def = dockById(id);
  if (!def) {
    throw new Error(`panelOptions: unknown dock id "${id}"`);
  }
  return {
    id: def.id,
    component: def.id,
    title: def.title,
    params: { ...def.params, __accent: def.accent ?? false },
    ...extra,
  };
}
