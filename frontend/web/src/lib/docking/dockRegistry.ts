import type { Component } from "svelte";
import type { AddPanelOptions } from "dockview-core";
import type { IconName } from "$lib/ui/Icon.svelte";
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
  // Identifies the dock where there is no room for its title -- the studio bar's
  // reopen buttons. The title still rides along as the button's accessible name.
  icon: IconName;
  component: Component<Record<string, unknown>>;
  params: Record<string, unknown>;
  accent?: boolean;
  // Narrowest width (px) this dock's content still renders at. Rides out as the
  // panel's `minimumWidth`, which Dockview clamps every split, distribute and
  // sash drag against, so no layout operation can crush the dock below it.
  // Absent = no floor beyond Dockview's own 100px group minimum.
  minWidth?: number;
}

export const DOCKS: DockDef[] = [
  // Default canvas preview. The tab carries a status dot + a `GLOBAL S/S` badge
  // (mock dockHead): the Default canvas renders the global scenes/sources, so its
  // header is labelled distinctly from the additional canvases' `OWN S/S` docks.
  // __-prefixed keys feed the custom tab and are stripped before the Svelte body.
  {
    id: "preview",
    title: "Preview · Main",
    icon: "video",
    component: PreviewDock,
    params: { __dot: "var(--color-muted)", __badge: "GLOBAL S/S" },
  },
  { id: "scenes", title: "Scenes", icon: "film", component: ScenesDock, params: {} },
  { id: "sources", title: "Sources", icon: "list", component: SourcesDock, params: {} },
  { id: "mixer", title: "Audio Mixer", icon: "audio-wave", component: AudioMixerDock, params: {} },
  { id: "transitions", title: "Transitions", icon: "transition", component: TransitionsDock, params: {} },
  {
    id: "multistream",
    title: "Multistream",
    icon: "destinations",
    component: MultistreamDock,
    params: {},
    accent: true,
  },
  // Merged read+send chat across every connected platform (Phase 9.0). Like
  // Multistream/Transitions it is NOT in the default layout -- it opens from the
  // CANVASES-bar reopen button -- but stays registered so it is addable/restorable.
  {
    id: "multichat",
    title: "Multichat",
    icon: "chat",
    component: MultichatDock,
    params: {},
    accent: true,
    minWidth: 280,
  },
  // Live cross-platform events feed (follows/subs/gifts/cheers/raids/superchats,
  // Phase 9.2). Like Multichat it is NOT in the default layout -- it opens from the
  // CANVASES-bar reopen button -- but stays registered so it is addable/restorable.
  { id: "events", title: "Events", icon: "bell", component: EventsDock, params: {}, accent: true, minWidth: 280 },
  // Per-account identity + audience (Channel-identity feature). Like Events/Multichat
  // it is meaningless without a logged-in account, so it is NOT in the default layout
  // and is OAuth-gated -- it opens from the CANVASES-bar reopen button once an account
  // is connected, but stays registered so it is addable/restorable.
  { id: "channels", title: "Channels", icon: "users", component: ChannelsDock, params: {}, minWidth: 280 },
  { id: "stats", title: "Stats", icon: "chart", component: StatsDock, params: {} },
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
    // Serialized with the layout, so the floor survives a restore without being
    // re-applied; Dockview reads it off whichever panel in a group is active.
    minimumWidth: def.minWidth,
    ...extra,
  };
}
