// Shared opener for the OAuth connect modal (device-code or PKCE browser flow).
// Mirrors missingFilesOpener: App owns the single OAuthConnectDialog mount gated on
// `.open`; the Streams tab requests it with the profile + provider via openOAuthConnect().

import { obs } from "./bridge";
import { suspendPreview } from "./previewGate.svelte";

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

// Hold the preview suspension across the dialog lifetime so the native overlay
// never raises above the modal.
let release: (() => void) | null = null;

// Set by the dialog once the connect flow has linked the account, so closing
// the (now-finished) modal does not abort the already-completed flow.
let connected = false;

/** Open the OAuth connect modal for one profile/provider. */
export function openOAuthConnect(req: OAuthConnectRequest): void {
  connected = false;
  oauthConnect.req = req;
  oauthConnect.open = true;
  release ??= suspendPreview();
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
  release?.();
  release = null;
}
