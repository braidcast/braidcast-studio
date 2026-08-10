import { obs } from "$lib/api/bridge";

// Session thumbnails are files on disk, not blobs, so the browser cannot resolve
// them and each one costs a file.readDataUri round trip. The history list reads one
// per row; the calendar can put the same session on screen in three views. Keyed by
// path so a thumbnail is read once per app run however many surfaces show it.
//
// A rejection is NOT cached: the file may simply not have been promoted yet (the
// recorder chooses one at stop), and caching that would leave the block blank for
// the rest of the session.
const inflight = new Map<string, Promise<string>>();

/** Resolves to a data URI, or "" when there is no thumbnail or the read failed --
 * a missing file is the fallback pattern, not an error worth surfacing. */
export function thumbDataUri(file: string): Promise<string> {
  if (!file) {
    return Promise.resolve("");
  }
  const cached = inflight.get(file);
  if (cached) {
    return cached;
  }
  const pending = obs
    .call("file.readDataUri", { path: file })
    .then((r) => r.dataUri)
    .catch(() => {
      inflight.delete(file);
      return "";
    });
  inflight.set(file, pending);
  return pending;
}
