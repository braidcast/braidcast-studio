// The overlay runtime's one log channel. console.error because obs-browser forwards nothing
// else to the session log -- which is why every line names what happened in words, so a policy
// notice (a preset fell back) still reads as one and not as a crash.

/** Distinct notices one page keeps. A source that runs for a whole broadcast would otherwise
 * grow this for every distinct key it ever logs; the oldest go first. */
const kMaxLoggedNotices = 32;
const logged = new Set<string>();

/** Log `line` once per `key` per page load. */
export function logOnce(key: string, line: string): void {
  if (logged.has(key)) {
    return;
  }
  // Oldest-first; a Set iterates in insertion order, and deleting the key being visited is
  // defined behaviour.
  for (const old of logged) {
    if (logged.size < kMaxLoggedNotices) {
      break;
    }
    logged.delete(old);
  }
  logged.add(key);
  console.error("OBSOverlay " + line);
}
