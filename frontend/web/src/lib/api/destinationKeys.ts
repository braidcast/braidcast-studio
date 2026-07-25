// Mirrors the host's OAuth::DestinationKey / Transports::*TransportId helpers
// (frontend/src/oauth/provider.hpp, frontend/src/events/transport_health.hpp).
//
// One builder rather than one per consumer: a destination key is also what
// `viewers.changed` puts in ViewerDestination.key, so chat health, event health
// and viewer counts all name the same destination the same way. Hand-rolling the
// "@" join per call site is how two YouTube destinations of one account came to
// share a key in the first place.

/**
 * The host's stable id for one live destination: an account under a stream
 * profile. Platforms with a single channel per account (Twitch, Kick) carry no
 * profile, so the key is the bare accountId -- an absent, empty or null
 * `profileUuid` all mean account-wide.
 *
 * Identical to `ViewerDestination.key`, so it doubles as the join key into a
 * `viewers.changed` payload.
 */
export function destinationKey(accountId: string, profileUuid?: string | null): string {
  return profileUuid ? `${accountId}@${profileUuid}` : accountId;
}

/**
 * Transport-health row id for a chat transport. Look rows up by this exact id --
 * never enumerate the health array to decide which destinations exist. `Stop()`
 * leaves a terminal `Disconnected` row behind on purpose, so a destination the
 * user has since unbound is still present for the rest of the session.
 */
export function chatTransportId(accountId: string, profileUuid?: string | null): string {
  return `chat:${destinationKey(accountId, profileUuid)}`;
}

/**
 * Transport-health row id for an event transport. Always account-wide: event
 * transports run on the account-connect lifecycle and never see a binding.
 */
export function eventsTransportId(accountId: string): string {
  return `events:${accountId}`;
}
