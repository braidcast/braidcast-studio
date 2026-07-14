// Shared opener for the OAuth connect modal (device-code or PKCE browser flow).
// Mirrors missingFilesOpener: App owns the single OAuthConnectDialog mount gated on
// `.open`; the Streams tab requests it with the profile + provider via openOAuthConnect().

import { obs } from "$lib/api/bridge";

/** The profile being connected + which provider drives the connect flow. */
export interface OAuthConnectRequest {
  profileUuid: string;
  providerId: string;
  platformName: string;
}

export const oauthConnect = $state<{ open: boolean; req: OAuthConnectRequest | null }>({
  open: false,
  req: null,
});

// Preview suspension is owned by Modal.svelte (OAuthConnectDialog wraps it), so the
// opener must NOT suspend too: a second, separately-released suspension raced the
// modal's on close and left the count stuck above zero, so the preview never re-showed.

// Set by the dialog once the connect flow has linked the account, so closing
// the (now-finished) modal does not abort the already-completed flow.
let connected = false;

/** Open the OAuth connect modal for one profile/provider. */
export function openOAuthConnect(req: OAuthConnectRequest): void {
  connected = false;
  oauthConnect.req = req;
  oauthConnect.open = true;
}

// openOAuthConnect() itself is a synchronous state flip (no obs.call), so there is no
// call-duration window to bracket with a local try/finally busy flag. The actual
// in-flight window is "the connect modal is open for this profile" — OAuthConnectDialog
// owns cancel-then-connect sequencing once mounted. Callers that want a busy-disabled
// reconnect button (immediate feedback + no duplicate-open) should key off this instead
// of inventing a parallel flag that can't reflect that window.
/** Whether the connect modal is currently open for this profile. */
export function isOAuthConnecting(profileUuid: string): boolean {
  return oauthConnect.open && oauthConnect.req?.profileUuid === profileUuid;
}

/** The dialog calls this on a successful link so the following close won't cancel. */
export function markOAuthConnected(): void {
  connected = true;
}

export function closeOAuthConnect(): void {
  // Abort an in-flight connect so a profile can't be linked after the modal is gone.
  // Skipped once the flow already connected. Swallow if the host bridge lacks the
  // method (older build).
  if (!connected) {
    void obs.call("oauth.cancelConnect").catch(() => {});
  }
  connected = false;
  oauthConnect.open = false;
  oauthConnect.req = null;
}
