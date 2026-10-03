// The bundled CC0 alert sounds (public/overlay/library/), as the editor reads them. The
// manifest is written by scripts/build-sound-library.py; a stored `sound` value names one of
// its entries as "library:<id>", which is why ids never change once shipped.

/** One manifest row. `file` is relative to the library directory. */
export interface LibrarySound {
  id: string;
  name: string;
  category: string;
  file: string;
  durationMs: number;
  source: string;
}

/** The category order the picker shows, and the only categories the manifest may use. */
export const SOUND_CATEGORIES = [
  "Chimes",
  "Coins & Rewards",
  "Fanfare & Jingles",
  "Arcade",
  "Whoosh & Swipe",
  "Impact",
] as const;

const LONG_CATEGORIES = new Set<string>(["Fanfare & Jingles"]);

/** The longest a sound in `category` may run: jingles get 6 s, everything else 3 s. */
export function soundCapMs(category: string): number {
  return LONG_CATEGORIES.has(category) ? 6000 : 3000;
}

export const LIBRARY_PREFIX = "library:";

/** The library id a stored sound value names, or null when it names an upload or nothing. */
export function librarySoundId(value: unknown): string | null {
  return typeof value === "string" && value.startsWith(LIBRARY_PREFIX) ? value.slice(LIBRARY_PREFIX.length) : null;
}

/** Where the editor itself loads the library from: the app's own bundle, so browsing and
 * previewing sounds needs neither the overlay server nor a widget. */
const MANIFEST_URL = "/overlay/library/sounds.json";
export const libraryFileUrl = (file: string): string => `/overlay/library/${file}`;

let manifest: Promise<LibrarySound[]> | null = null;

/** The manifest, fetched once per editor session. A failed fetch is not cached, so the next
 * open of the picker tries again. */
export function loadSoundLibrary(): Promise<LibrarySound[]> {
  manifest ??= fetch(MANIFEST_URL)
    .then((r) => {
      if (!r.ok) {
        throw new Error(`sound library unavailable (${r.status})`);
      }
      return r.json() as Promise<LibrarySound[]>;
    })
    .catch((e: unknown) => {
      manifest = null;
      throw e;
    });
  return manifest;
}
