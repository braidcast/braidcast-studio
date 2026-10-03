// Uploads from a scoped field: what the file is called, which cap it falls under, and where
// the editor plays it back from. The host names the file after the key it is given, so the
// key carries the scope -- the same field uploaded in Defaults and in Bits are two files,
// not one overwriting the other.

export type AssetKind = "sound" | "image" | "video";

/** The per-kind caps the host enforces (overlays.assetLimits), in bytes. */
export type AssetLimits = Record<AssetKind, number>;

/** A stored asset reference's file name, or null when the value is not an upload. */
export function assetFileOf(value: unknown): string | null {
  return typeof value === "string" && value.startsWith("assets/") ? value.slice("assets/".length) : null;
}

/** The extension a file keeps on the host: lowercased, letters and digits only. */
function extensionOf(name: string): string {
  const dot = name.lastIndexOf(".");
  const ext = dot >= 0 ? name.slice(dot + 1).toLowerCase() : "";
  return /^[a-z0-9]{1,5}$/.test(ext) ? ext : "bin";
}

/** "<scope>-<field>.<ext>": `default-sound.ogg`, `cheer-media.webm`, `v_8f2c-media.gif`. A
 * re-upload with another extension is a second file; the first one goes when the scope is
 * pruned after the save that stopped naming it (overlays.removeScopeAssets). */
export function scopedAssetKey(scope: string, fieldKey: string, fileName: string): string {
  return `${scope}-${fieldKey}.${extensionOf(fileName)}`;
}

/** Which cap a file falls under: WebM is video, any other image MIME is image, audio is
 * sound. Null for a type no media or sound field takes. */
export function assetKindOf(file: { type: string; name: string }): AssetKind | null {
  if (file.type === "video/webm" || /\.webm$/i.test(file.name)) {
    return "video";
  }
  if (file.type.startsWith("image/")) {
    return "image";
  }
  if (file.type.startsWith("audio/")) {
    return "sound";
  }
  return null;
}

const KIND_LABEL: Record<AssetKind, string> = { sound: "Sounds", image: "Images", video: "Videos" };

const mb = (bytes: number): string => `${Math.round((bytes / (1024 * 1024)) * 10) / 10} MB`;

/** Why `file` cannot be uploaded, said before any of it is read, or null when it can. */
export function uploadProblem(file: { size: number }, kind: AssetKind, limits: AssetLimits): string | null {
  const cap = limits[kind];
  return file.size > cap ? `${KIND_LABEL[kind]} can be up to ${mb(cap)}. This file is ${mb(file.size)}.` : null;
}

/** Where the editor plays an upload from: the widget's own asset route on the loopback
 * server, token and all, built off the widget URL the host handed back. */
export function assetPreviewUrl(widgetUrl: string, file: string): string {
  const u = new URL(widgetUrl);
  const token = u.searchParams.get("t") ?? "";
  return `${u.origin}${u.pathname}/assets/${encodeURIComponent(file)}?t=${encodeURIComponent(token)}`;
}

let uploadsInFlight = 0;

/** Run an upload -- the transfer and the setting that names its file -- counted while it is
 * in flight. A scope prune must not run inside that window: the host records the new file
 * before the editor's setting names it, so a prune then would delete what was just
 * uploaded. */
export async function trackUpload<T>(run: () => Promise<T>): Promise<T> {
  uploadsInFlight++;
  try {
    return await run();
  } finally {
    uploadsInFlight--;
  }
}

/** Whether any upload is between its transfer and the setting that names its file. */
export function uploadInFlight(): boolean {
  return uploadsInFlight > 0;
}

/** Bytes as the media picker's size line shows them. */
export function formatBytes(bytes: number): string {
  return bytes >= 1024 * 1024 ? mb(bytes) : `${Math.max(1, Math.round(bytes / 1024))} KB`;
}
