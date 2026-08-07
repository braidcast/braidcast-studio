// One read-only sweep of the loaded scene collection, shaped for the Config
// Advisor's detectors. Every rule runs against this single snapshot rather than
// issuing its own bridge calls, so adding a rule costs no extra round trips.
//
// The sweep is NOT cheap enough for the 1 Hz stats tick: source settings are one
// `properties.get` per source (there is no batch getter). advisorStore runs it on
// mount and on the events that invalidate it, never per stats sample.
//
// THE INVARIANT THIS FILE EXISTS TO HOLD: every read either produces a value or
// records a gap. There is no third outcome. A rejection absorbed as `[]` is
// indistinguishable from a genuinely empty result, and the rules that make NEGATIVE
// claims ("this source is in no scene", "this one is not on air") would turn that
// silence into an accusation. `read()` below is the only way to call the bridge here.

import { obs } from "$lib/api/bridge";
import type { SceneItem } from "$lib/api/bridge";
import { canvasStore } from "$lib/stores/canvasStore.svelte";
import { Cat } from "$lib/utils/logCategories";
import { log } from "$lib/utils/log";

/** Source type ids the advisor reasons about by name. Same vocabulary as
 * SceneItem.typeId / ExistingSource.typeId. */
export const SOURCE_TYPE = {
  browser: "browser_source",
  group: "group",
  scene: "scene",
} as const;

/** One scene the sweep could actually enumerate, with its top-level members. */
export interface ScannedScene {
  /** Owning canvas uuid; "" for the Default canvas (the global channel-0 path). */
  canvasUuid: string;
  canvasName: string;
  name: string;
  /** Bound to that canvas's channel 0 right now — i.e. on air. */
  current: boolean;
  items: SceneItem[];
}

export interface AdvisorSnapshot {
  scenes: ScannedScene[];
  /** Source name -> type id, for every input/group source in the collection.
   * Scenes are excluded: they are containers here, not placeable sources. */
  sources: Map<string, string>;
  /** Source name -> its obs settings values, only for the type ids the registry
   * asked for (AdvisorRule.needsSettings). */
  settings: Map<string, Record<string, unknown>>;
  /** Source name -> the distinct page-error messages seen in this session's log. */
  pageErrors: Map<string, string[]>;
  /** Names of sources sitting in a scene that is current on some canvas. */
  onAir: Set<string>;
  /** Names of sources reachable in ANY scanned scene. */
  placed: Set<string>;
  /** Why `onAir` is not provably the whole on-air set; empty when it is. Only
   * regions REACHED FROM a current scene count — an additional canvas's non-current
   * scenes cannot change what is showing, so they must not suppress an on-air claim. */
  onAirGaps: string[];
  /** Why `placed` is not provably the whole placement set; empty when it is. A
   * superset of `onAirGaps`: a claim about the whole collection needs the whole
   * collection, so any region unreadable anywhere counts. */
  graphGaps: string[];
  /** Why `settings` is not provably complete for the requested types. */
  settingsGaps: string[];
  /** Why `pageErrors` is not provably the whole error set. */
  logGaps: string[];
}

/** The reasons a read cannot be answered, phrased for the panel: each completes the
 * sentence "this check could not run because …". */
const GAP = {
  /** ResolveTargetScene (frontend/src/bridge.cpp) ignores the `scene` argument for
   * a non-Default canvas and always resolves that canvas's CURRENT scene, so its
   * other scenes are not addressable from the frontend at all. */
  extraCanvasScenes: "an additional canvas holds scenes this app cannot address",
  /** obs_scene_enum_items does not descend into groups and no bridge method
   * enumerates their members. */
  group: "a group's contents cannot be listed",
  unresolvedNesting: "a nested scene could not be matched to a scanned scene",
  canvasListFailed: "the canvas list could not be read",
  sceneListFailed: "the scene list could not be read",
  sceneReadFailed: "a scene's contents could not be read",
  sourceListFailed: "the source list could not be read",
  noSceneToScanFrom: "this collection has no scene to enumerate its sources from",
  settingsReadFailed: "a source's settings could not be read",
  logReadFailed: "the session log could not be read",
} as const;

/** The four independent questions the rules ask, each with its own channel. A failed
 * read writes into every channel whose answer it would have changed — never into
 * none. The helpers exist so a call site names the blast radius rather than picking
 * channels by hand and getting one wrong. */
class Gaps {
  readonly onAir = new Set<string>();
  readonly graph = new Set<string>();
  readonly settings = new Set<string>();
  readonly log = new Set<string>();

  /** Placement became unknowable, on air and collection-wide. */
  placement(reason: string): void {
    this.onAir.add(reason);
    this.graph.add(reason);
  }

  /** Collection-wide placement only — a region that by definition cannot be on air,
   * so it must not suppress an on-air claim. */
  collectionOnly(reason: string): void {
    this.graph.add(reason);
  }

  /** The source universe underpins every rule, including the name filter rule 1
   * applies to its parsed log errors — lose it and every answer is short. */
  universe(reason: string): void {
    this.placement(reason);
    this.settings.add(reason);
    this.log.add(reason);
  }
}

/** The one way this module calls the bridge: resolve to a value, or record a gap and
 * return null. Callers must branch on null — that is the point (see the file header). */
async function read<T>(what: string, call: Promise<T>, onFail: () => void): Promise<T | null> {
  try {
    return await call;
  } catch (e) {
    log.dbg(Cat.scene, `advisor read failed (${what}):`, (e as Error).message);
    onFail();
    return null;
  }
}

// The exact line obs-browser writes for a page-level console error, from
// BrowserClient::OnConsoleMessage (plugins/obs-browser/browser-client.cpp):
//
//   blog(level, "[obs-browser: '%s'] %s: %s (%s:%d)", sourceName, code, message, source, line)
//
// STRING CONTRACT: this parse and that format string are one pair, and the fork owns
// both sides — changing either without the other silently empties this rule. Only
// Error and Fatal are matched because only those two reach the log; the Info case
// returns early before the blog call.
const PAGE_ERROR_RE = /\[obs-browser: '(.+?)'\] (?:Error|Fatal): (.+)$/;
// Trailing " (<url>:<line>)" appended by the same format string. Stripped so the
// same failure at two line numbers dedupes into one message.
const PAGE_ERROR_LOCATION_RE = /^(.*) \(\S*:\d+\)$/;
// A page in a reload loop can log thousands of lines; keep enough distinct messages
// to diagnose with and no more.
const MAX_MESSAGES_PER_SOURCE = 4;

/** Group the `[obs-browser: '…'] Error:` lines of a log tail by source name,
 * keeping each source's distinct messages in first-seen order. */
function parseBrowserPageErrors(contents: string): Map<string, string[]> {
  const bySource = new Map<string, string[]>();
  for (const line of contents.split("\n")) {
    const m = PAGE_ERROR_RE.exec(line);
    if (!m) {
      continue;
    }
    const name = m[1];
    const message = (PAGE_ERROR_LOCATION_RE.exec(m[2].trim())?.[1] ?? m[2]).trim();
    let seen = bySource.get(name);
    if (!seen) {
      seen = [];
      bySource.set(name, seen);
    }
    if (seen.length < MAX_MESSAGES_PER_SOURCE && !seen.includes(message)) {
      seen.push(message);
    }
  }
  return bySource;
}

/** Enumerate every scene the frontend can address, with its top-level members. */
async function scanScenes(gaps: Gaps): Promise<ScannedScene[]> {
  await canvasStore.whenReady();
  // whenReady() resolves from canvasStore's `finally`, so it settles even when
  // canvas.list REJECTED and the list is empty. Without this check the sweep would
  // read an empty canvas list as "no additional canvases" and report their on-air
  // sources as off-air waste.
  if (canvasStore.error !== null) {
    gaps.placement(GAP.canvasListFailed);
  }
  const canvases = canvasStore.canvases;
  const scenes: ScannedScene[] = [];

  // Default canvas: every scene in the collection is addressable by name. Losing the
  // list hides the program scene too, so this is a placement gap on both channels.
  const defaultName = canvases.find((c) => c.isDefault)?.name ?? "Default";
  const defaultScenes = await read("scenes.list", obs.call("scenes.list"), () =>
    gaps.placement(GAP.sceneListFailed),
  );
  const defaultList = defaultScenes ?? [];
  const defaultItems = await Promise.all(
    defaultList.map((s) =>
      read(`sceneItems.list '${s.name}'`, obs.call("sceneItems.list", { scene: s.name }), () => {
        gaps.collectionOnly(GAP.sceneReadFailed);
        if (s.current) {
          gaps.onAir.add(GAP.sceneReadFailed);
        }
      }),
    ),
  );
  defaultList.forEach((s, i) => {
    const items = defaultItems[i];
    if (items) {
      scenes.push({ canvasUuid: "", canvasName: defaultName, name: s.name, current: s.current, items });
    }
  });

  for (const c of canvases) {
    if (c.isDefault) {
      continue;
    }
    // A rejection here is NOT "this canvas has no scenes": its channel-0 scene is
    // live, so its members would silently vanish from `onAir` while the rule that
    // reads `onAir` kept reporting. Both channels.
    const list = await read(`scenes.list '${c.name}'`, obs.call("scenes.list", { canvas: c.uuid }), () =>
      gaps.placement(GAP.sceneListFailed),
    );
    if (!list) {
      continue;
    }
    // Only the current scene is reachable. The canvas's OTHER scenes are a
    // collection-wide gap and not an on-air one — a scene off channel 0 cannot
    // affect what is showing.
    const current = list.find((s) => s.current);
    if (list.length > (current ? 1 : 0)) {
      gaps.collectionOnly(GAP.extraCanvasScenes);
    }
    if (!current) {
      continue;
    }
    const items = await read(`sceneItems.list '${c.name}'`, obs.call("sceneItems.list", { canvas: c.uuid }), () =>
      gaps.placement(GAP.sceneReadFailed),
    );
    if (!items) {
      continue;
    }
    scenes.push({ canvasUuid: c.uuid, canvasName: c.name, name: current.name, current: true, items });
  }

  return scenes;
}

/** Name -> type id for every input/group source in the collection.
 * `sources.listExisting` reports all of them EXCEPT the target scene's own members,
 * so union it with that scene's members — whose type the sceneItems.list row now
 * carries — to cover a source that sits in every scene. */
async function scanSources(scenes: ScannedScene[], gaps: Gaps): Promise<Map<string, string>> {
  const sources = new Map<string, string>();
  const ref = scenes.find((s) => s.canvasUuid === "" && s.current) ?? scenes[0];
  if (!ref) {
    // No scene means no anchor for sources.listExisting, so the collection's sources
    // cannot be enumerated at all — including the unplaced ones rule 4 exists to find.
    gaps.universe(GAP.noSceneToScanFrom);
    return sources;
  }
  // ResolveTargetScene answers "no scene" when the anchor is deleted mid-burst
  // (frontend/src/bridge.cpp). Swallowing that as an empty universe would shrink
  // every rule's subject list AND drop real page-error findings through the
  // sources.has() filter below, so it gaps all four channels.
  const existing = await read(
    "sources.listExisting",
    obs.call("sources.listExisting", { scene: ref.name, canvas: ref.canvasUuid }),
    () => gaps.universe(GAP.sourceListFailed),
  );
  for (const e of existing ?? []) {
    sources.set(e.name, e.typeId);
  }
  for (const scene of scenes) {
    for (const item of scene.items) {
      // A nested scene is a container, not a placeable source: obs_enum_sources
      // leaves scenes out, so counting them here would flag every top-level scene
      // as an unplaced source.
      if (item.source && item.typeId !== SOURCE_TYPE.scene) {
        sources.set(item.source, item.typeId);
      }
    }
  }
  return sources;
}

/** One `properties.get` per source of a requested type. There is no batch getter;
 * the set is small (the registry declares which types it reads) and this runs on
 * mount plus invalidating events only. */
async function scanSettings(
  sources: Map<string, string>,
  typeIds: ReadonlySet<string>,
  gaps: Gaps,
): Promise<Map<string, Record<string, unknown>>> {
  const settings = new Map<string, Record<string, unknown>>();
  if (typeIds.size === 0) {
    return settings;
  }
  const wanted = [...sources].filter(([, typeId]) => typeIds.has(typeId)).map(([name]) => name);
  const values = await Promise.all(
    wanted.map((name) =>
      // A rule cannot answer for a subject whose settings it never read, and
      // omitting that subject silently is the same unearned all-clear one level down.
      read(
        `properties.get '${name}'`,
        obs.call("properties.get", { kind: "source", ref: name }).then((r) => r.values),
        () => gaps.settings.add(GAP.settingsReadFailed),
      ),
    ),
  );
  wanted.forEach((name, i) => {
    const v = values[i];
    if (v) {
      settings.set(name, v);
    }
  });
  return settings;
}

/** This session's browser page errors, keyed by source name. */
async function scanPageErrors(sources: Map<string, string>, gaps: Gaps): Promise<Map<string, string[]>> {
  const session = await read("log.getCurrent", obs.call("log.getCurrent"), () => gaps.log.add(GAP.logReadFailed));
  if (!session) {
    return new Map();
  }
  if (session.path === "") {
    // The host reports an empty path when it has no session log file to read, and
    // the empty contents that come with it are not evidence of a clean session.
    gaps.log.add(GAP.logReadFailed);
    return new Map();
  }
  // Only the current collection's sources exist in libobs, and the log tail spans the
  // whole session: drop errors attributed to a name that is gone (deleted or renamed)
  // rather than showing a finding nothing can act on.
  const parsed = parseBrowserPageErrors(session.contents);
  return new Map([...parsed].filter(([name]) => sources.has(name)));
}

/** Read the whole advisor input set. `settingsTypeIds` is the union of every rule's
 * `needsSettings`, passed in by the store so this module never imports the registry. */
export async function buildSnapshot(settingsTypeIds: ReadonlySet<string>): Promise<AdvisorSnapshot> {
  const gaps = new Gaps();
  const scenes = await scanScenes(gaps);
  const sources = await scanSources(scenes, gaps);
  const settings = await scanSettings(sources, settingsTypeIds, gaps);
  const pageErrors = await scanPageErrors(sources, gaps);

  const sceneKey = (canvasUuid: string, name: string): string => `${canvasUuid}|${name}`;
  const byKey = new Map(scenes.map((s) => [sceneKey(s.canvasUuid, s.name), s]));

  // Whole-collection placement. Anything unreadable ANYWHERE counts against it.
  const placed = new Set<string>();
  for (const scene of scenes) {
    for (const item of scene.items) {
      if (item.typeId === SOURCE_TYPE.group) {
        gaps.collectionOnly(GAP.group);
      }
      if (!item.source) {
        continue;
      }
      placed.add(item.source);
      if (item.typeId === SOURCE_TYPE.scene && !byKey.has(sceneKey(scene.canvasUuid, item.source))) {
        gaps.collectionOnly(GAP.unresolvedNesting);
      }
    }
  }

  // On-air membership has to follow nesting: a scene placed inside the current scene
  // renders with it, so its members are showing too. Counting only the current
  // scene's own top-level items would read a browser source one level down as off
  // air — exactly the false positive the off-air rule must never produce. Only what
  // this walk touches can gap the on-air set.
  const onAir = new Set<string>();
  const walk = (scene: ScannedScene, seen: Set<string>): void => {
    for (const item of scene.items) {
      if (item.typeId === SOURCE_TYPE.group) {
        gaps.placement(GAP.group);
      }
      if (!item.source) {
        continue;
      }
      onAir.add(item.source);
      if (item.typeId !== SOURCE_TYPE.scene) {
        continue;
      }
      const key = sceneKey(scene.canvasUuid, item.source);
      if (seen.has(key)) {
        continue;
      }
      seen.add(key);
      const nested = byKey.get(key);
      if (!nested) {
        gaps.placement(GAP.unresolvedNesting);
        continue;
      }
      walk(nested, seen);
    }
  };
  for (const scene of scenes) {
    if (scene.current) {
      walk(scene, new Set([sceneKey(scene.canvasUuid, scene.name)]));
    }
  }

  return finish({
    scenes,
    sources,
    settings,
    pageErrors,
    onAir,
    placed,
    onAirGaps: [...gaps.onAir],
    graphGaps: [...gaps.graph],
    settingsGaps: [...gaps.settings],
    logGaps: [...gaps.log],
  });
}

function finish(snapshot: AdvisorSnapshot): AdvisorSnapshot {
  log.dbg(
    Cat.scene,
    "advisor sweep",
    `scenes=${snapshot.scenes.length}`,
    `sources=${snapshot.sources.size}`,
    `settings=${snapshot.settings.size}`,
    `pageErrors=${snapshot.pageErrors.size}`,
    `onAirGaps=[${snapshot.onAirGaps.join("; ")}]`,
    `graphGaps=[${snapshot.graphGaps.join("; ")}]`,
    `settingsGaps=[${snapshot.settingsGaps.join("; ")}]`,
    `logGaps=[${snapshot.logGaps.join("; ")}]`,
  );
  return snapshot;
}
