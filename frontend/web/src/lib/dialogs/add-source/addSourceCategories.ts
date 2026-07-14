import type { SourceType } from "$lib/api/bridge";

export type Category = "capture" | "media" | "audio" | "web" | "other";

/** Fixed display order of the functional categories (Recently Used is prepended
 * by the modal, not here). */
export const CATEGORY_ORDER: Category[] = ["capture", "media", "audio", "web", "other"];

export const CATEGORY_LABEL: Record<Category | "recent", string> = {
  recent: "Recently Used",
  capture: "Capture",
  media: "Media",
  audio: "Audio",
  web: "Web",
  other: "Other",
};

/** Curated id -> category. Windows-primary ids plus macOS/Linux audio variants;
 * anything unmapped falls through to "other". One line per type = a data edit to
 * add/move a source. */
const TYPE_CATEGORY: Record<string, Category> = {
  // Capture
  monitor_capture: "capture",
  window_capture: "capture",
  game_capture: "capture",
  dshow_input: "capture",
  // Media
  ffmpeg_source: "media",
  image_source: "media",
  slideshow: "media",
  color_source: "media",
  color_source_v3: "media",
  text_gdiplus: "media",
  text_gdiplus_v2: "media",
  text_ft2_source: "media",
  text_ft2_source_v2: "media",
  // Audio
  wasapi_input_capture: "audio",
  wasapi_output_capture: "audio",
  wasapi_process_output_capture: "audio",
  coreaudio_input_capture: "audio",
  coreaudio_output_capture: "audio",
  pulse_input_capture: "audio",
  pulse_output_capture: "audio",
  // Web
  browser_source: "web",
};

export function categoryOf(typeId: string): Category {
  return TYPE_CATEGORY[typeId] ?? "other";
}

/** Group types into their categories, preserving the input order within each and
 * dropping empty categories. Returns entries in CATEGORY_ORDER. */
export function bucketTypes(types: SourceType[]): { category: Category; types: SourceType[] }[] {
  const map = new Map<Category, SourceType[]>();
  for (const t of types) {
    const c = categoryOf(t.id);
    const arr = map.get(c);
    if (arr) {
      arr.push(t);
    } else {
      map.set(c, [t]);
    }
  }
  return CATEGORY_ORDER.filter((c) => map.has(c)).map((c) => ({ category: c, types: map.get(c)! }));
}
