// Which transport carries a destination's chat or its events, and what its state is
// called.
//
// Shared because three surfaces ask the same question of the same row: the Chat dock's
// destination chip, the Events dock's, and the Stats dock's per-output line. A second
// copy of the wording is how one surface came to read "chat stopped" while the other
// read "failed" for the transport they were both describing.
//
// Its own module rather than exports on a dock: a dock renders transport health, it
// does not define what a transport is. Colors stay in stateColors.ts
// (TRANSPORT_STATE_COLOR) -- state, wording and color are three maps over one enum, and
// keeping the color with the rest of the palette is what stops a fourth appearing.

import { chatTransportId, eventsTransportId } from "$lib/api/destinationKeys";
import { transportHealthStore } from "$lib/stores/transportHealthStore.svelte";
import type { TransportHealth, TransportHealthState } from "$lib/api/bridge";
import type { DestinationIdentity } from "$lib/stores/destinationIdentityStore.svelte";

/** A destination's chat transport together with the health row that carries it. The
 * row is present by construction -- resolution IS the row lookup -- so a consumer
 * never has to default an absent state. */
export interface ChatTransport {
  id: string;
  /** Null when the chat is account-wide -- the send contract's "no profileUuid". */
  profileUuid: string | null;
  row: TransportHealth;
}

/**
 * The transport carrying this destination's chat, found by EXACT id and never by
 * scanning the health array: Stop() leaves a terminal Disconnected row behind, so the
 * array still lists destinations the user has unbound since. Platforms that run one
 * chat per broadcast key on (account, profile); platforms with a single chat per
 * channel key on the account alone, and the pair of lookups is what tells them apart
 * without a caller holding a per-platform table of its own.
 *
 * Null means there is NO chat here -- a key/RTMP/WHIP profile, or an account that has
 * never opened one. That is not "disconnected": a caller must be able to say nothing
 * rather than report a state nobody measured.
 */
export function chatTransportFor(d: DestinationIdentity): ChatTransport | null {
  // Only an account-backed profile can own a chat transport, and the empty accountId a
  // key/RTMP profile carries would otherwise probe the ids "chat:@<uuid>" / "chat:".
  if (!d.accountId) {
    return null;
  }
  const perBroadcast = transportHealthStore.byId.get(chatTransportId(d.accountId, d.profileUuid));
  if (perBroadcast) {
    return { id: perBroadcast.id, profileUuid: d.profileUuid, row: perBroadcast };
  }
  const accountWide = transportHealthStore.byId.get(chatTransportId(d.accountId));
  if (accountWide) {
    return { id: accountWide.id, profileUuid: null, row: accountWide };
  }
  return null;
}

/**
 * The transport carrying this destination's events. Always account-wide -- event
 * transports run on the account-connect lifecycle and never see a binding -- so unlike
 * chat there is no per-broadcast row to prefer, and the health row IS the whole answer.
 *
 * Null carries the same meaning as above: no transport was ever opened here, which is
 * not the same claim as "disconnected".
 */
export function eventsTransportFor(d: DestinationIdentity): TransportHealth | null {
  if (!d.accountId) {
    return null;
  }
  return transportHealthStore.byId.get(eventsTransportId(d.accountId)) ?? null;
}

/** The state in words, because a chip's status edge is a color and a color cannot be
 * the only carrier. DestinationChips appends the note to both title and aria-label; the
 * Stats dock prints it inline on the output row.
 *
 * One builder rather than a table per subject: chat and events are the same five states
 * read against different nouns, and two hand-written tables drift a phrase at a time.
 * `stopped` is neutral on purpose -- it covers a broadcast that simply ended as well as
 * a genuine fault, and the caller appends its own reason, which carries the judgement. */
function stateNotes(subject: string): Record<TransportHealthState, string> {
  return {
    connected: `${subject} connected`,
    connecting: `${subject} connecting`,
    reconnecting: `${subject} reconnecting`,
    failed: `${subject} stopped`,
    disconnected: `${subject} not connected`,
  };
}

export const CHAT_STATE_NOTE = stateNotes("chat");
export const EVENTS_STATE_NOTE = stateNotes("events");
