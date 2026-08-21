// The one wording for what an open CEF remote-debugging port exposes. The title-bar
// badge and the Diagnostics tab describe the same risk, and a milder phrasing in
// either place is the one an operator would believe -- so both read this, and neither
// gets to soften it locally.

/** What any other process on this machine gets while the port is listening. */
export const DEVTOOLS_EXPOSURE =
  "Any program running on this computer can attach to it and execute JavaScript inside Braidcast, " +
  "which can read your connected accounts' sign-in tokens and your stream keys.";

/** The env opt-in that opened it. Both variables are required -- Resolve() wants
 * BRAIDCAST_DEBUG truthy AND the 'devtools' token -- so naming only the token would
 * send someone off to set half of it, get no port, and conclude the indicator lies.
 * Clearing EITHER variable closes it. */
export const DEVTOOLS_OPT_IN = "BRAIDCAST_DEBUG=1 BRAIDCAST_DEBUG_COMPONENTS=devtools";

/** Port + exposure in one sentence, for a hover/description label. */
export function devToolsExposureLabel(port: number): string {
  return `Remote debugging port ${port} is open. ${DEVTOOLS_EXPOSURE}`;
}
