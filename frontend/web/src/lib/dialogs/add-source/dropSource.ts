import { obs } from "$lib/api/bridge";
import { sourceSelection } from "$lib/stores/sourceSelectionStore.svelte";

// Turns an OS/text drop into a source, reusing the same bridge seams AddSource does:
// sources.create for the object, properties.set to load the path/url/text into it.
// A plan's `confirm`, when present, is a question the caller must accept first (§1.6
// requires a prompt before a dropped URL becomes a browser source).
export interface DropPlan {
  type: string;
  name: string;
  settings: Record<string, unknown>;
  confirm?: string;
}

// Extension (lowercase, no dot) -> source type + the settings key that takes the
// absolute OS path. One row per family = a data edit to support a new format; the
// list is flattened to a lookup Map at load so classification is a single get.
interface FileRule {
  exts: string[];
  type: string;
  pathKey: string;
  extra?: Record<string, unknown>;
}
const FILE_RULES: FileRule[] = [
  { exts: ["png", "jpg", "jpeg", "gif", "webp", "bmp", "tga", "jxr", "avif"], type: "image_source", pathKey: "file" },
  {
    exts: ["mp4", "mkv", "mov", "webm", "m4v", "flv", "avi", "ts", "mpg", "mpeg", "wmv", "mp3", "aac", "flac",
           "wav", "ogg", "opus", "m4a", "wma"],
    type: "ffmpeg_source",
    pathKey: "local_file",
    extra: { is_local_file: true },
  },
];
const EXT_RULE = new Map<string, FileRule>();
for (const rule of FILE_RULES) {
  for (const ext of rule.exts) {
    EXT_RULE.set(ext, rule);
  }
}

// CEF exposes the OS path on a dropped File via the non-standard File.path; a
// sandboxed/plain-browser build lacks it, so local-file drops are a no-op there
// (an absolute path cannot be synthesized from a sandboxed File).
type PathFile = File & { path?: string };

function extOf(path: string): string {
  const dot = path.lastIndexOf(".");
  return dot >= 0 ? path.slice(dot + 1).toLowerCase() : "";
}
function baseName(path: string): string {
  const seg = path.split(/[\\/]/).pop() || path;
  const dot = seg.lastIndexOf(".");
  return dot > 0 ? seg.slice(0, dot) : seg;
}

/** A dropped OS file -> its image/media source plan, or null for an unknown extension
 *  or a File whose OS path this build does not expose. */
export function planFile(file: PathFile): DropPlan | null {
  const path = file.path;
  if (!path) {
    return null;
  }
  const rule = EXT_RULE.get(extOf(path));
  if (!rule) {
    return null;
  }
  return { type: rule.type, name: baseName(path) || "Media", settings: { [rule.pathKey]: path, ...rule.extra } };
}

// A dropped string is a browser source when it parses as an http(s)/file URL or ends
// in .html/.htm; everything else becomes a text source.
function isBrowserText(text: string): boolean {
  return /^(https?|file):\/\//i.test(text) || /\.html?([?#].*)?$/i.test(text);
}

// The text source id varies by platform and version (gdiplus on Windows, freetype2
// elsewhere) and deprecated variants are hidden from sourceTypes.list. Resolve
// against the same list AddSource uses, preferring the newest variant; cache it after
// the first text drop.
const TEXT_CANDIDATES = ["text_gdiplus_v3", "text_gdiplus_v2", "text_gdiplus", "text_ft2_source_v2", "text_ft2_source"];
let textTypeCache: string | null = null;
async function resolveTextType(): Promise<string | null> {
  if (textTypeCache) {
    return textTypeCache;
  }
  const list = await obs.call("sourceTypes.list");
  const ids = new Set(list.map((t) => t.id));
  textTypeCache = TEXT_CANDIDATES.find((id) => ids.has(id)) ?? null;
  return textTypeCache;
}

/** A dropped string -> a browser (needs confirm) or text source plan; null when the
 *  string is empty or no text source type is registered on this platform. */
export async function planText(text: string): Promise<DropPlan | null> {
  const trimmed = text.trim();
  if (!trimmed) {
    return null;
  }
  if (isBrowserText(trimmed)) {
    return {
      type: "browser_source",
      name: "Browser",
      settings: { url: trimmed },
      confirm: `Create a browser source for ${trimmed}?`,
    };
  }
  const type = await resolveTextType();
  if (!type) {
    return null;
  }
  const label = trimmed.split("\n")[0].slice(0, 24).trim() || "Text";
  return { type, name: label, settings: { text: trimmed } };
}

// sources.create rejects a duplicate name; mirror OBS' " N" de-dup by retrying with a
// numeric suffix on a name-clash error. Bounded so a genuine failure still surfaces.
const NAME_CLASH = /already exists/i;
async function createUnique(type: string, base: string): Promise<{ id: number; source: string }> {
  // Default-canvas current scene (SourcesDock publishes it); null lets the bridge
  // resolve the active scene so a drop still lands when no dock owns the selection.
  const scene = sourceSelection.scene;
  for (let n = 1; n <= 50; n++) {
    const name = n === 1 ? base : `${base} ${n}`;
    try {
      return await obs.call("sources.create", { type, name, canvas: null, scene });
    } catch (e) {
      if (n < 50 && NAME_CLASH.test((e as Error).message)) {
        continue;
      }
      throw e;
    }
  }
  throw new Error("could not find a free source name");
}

/** Create the planned source in the Default-canvas current scene and load its
 *  path/url/text via properties.set. Returns the created source name; the bridge
 *  error propagates on failure. */
export async function createDropped(plan: DropPlan): Promise<string> {
  const created = await createUnique(plan.type, plan.name);
  if (Object.keys(plan.settings).length > 0) {
    await obs.call("properties.set", { kind: "source", ref: created.source, settings: plan.settings });
  }
  return created.source;
}
