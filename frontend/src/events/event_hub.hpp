#ifndef OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_HUB_HPP_
#define OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_HUB_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "event_store.hpp"
#include "event_transport.hpp"
#include "../chat/chat_archive.hpp" // Chat::Redaction

// Forward-declare OAuthAccount (defined in oauth/provider.hpp, already pulled in by
// event_transport.hpp) so this header names it without depending on include order.
namespace OAuth {
struct OAuthAccount;
}

// The EventHub (Phase 9.2a): owns the set of live per-platform event transports on
// the ACCOUNT-CONNECT lifecycle (not go-live, unlike ChatHub). Per connected
// account with a makeEvents() transport it runs one detached worker that (1) seeds the
// store via backfill (emitting one events.backfill batch), then (2) drives the
// transport's real-time connect() plus an optional poll() cadence. Normalized
// events funnel through Ingest -> dedupe in the shared EventStore -> alive-guarded
// PostToUi + EmitEvent("events.new").
//
// Thread-safety mirrors ChatHub exactly: the active map is mutex-guarded, each
// generation carries a shared_ptr<atomic<bool>> cancel flag so a re-Start never
// un-cancels an old worker, and the singleton (Events::Hub()) outlives its detached
// workers to process exit (they capture it raw, which is therefore safe).
namespace Events {

// Moderator removals seen lately, so an event that reaches the store after its removal (the
// Super Chat REST poll's copy, up to a poll later; an EventSub cheer racing its cheerer's
// ban) does not carry the words that removal took. Ids, bounds and times only: what is kept
// is a Chat::Redaction, which has no field for text. Bounded to the last kMax removals and
// kKeepMs, in memory only. Thread-safe.
class RecentRemovals {
public:
	static constexpr size_t kMax = 512;
	static constexpr int64_t kKeepMs = 15 * 60 * 1000;
	// How far past the moment we read a ban with no platform time it still reaches. An
	// event's time is the platform's (an EventSub notification's, a Super Chat's), ours is
	// the moment the ban was read, and the event can be stamped after the ban it preceded.
	static constexpr int64_t kSeenMarginMs = 30 * 1000;

	// Remember `op` on `dest`, seen at `seenMs`. A ban or timeout read live carries no
	// `beforeTs`; it reaches an event of that viewer stamped at or before the time the
	// platform gave the ban (`happenedAt`) or, with none, at or before `seenMs` plus
	// kSeenMarginMs. A clear of the whole chat is not kept; it reaches no event.
	void Remember(const OAuth::DestinationId &dest, const Chat::ModerationOp &op, int64_t seenMs);
	// Take the words off `ev`, marking it deleted, when a removal seen within kKeepMs of
	// `nowMs` reaches it by RedactModeratedEvents' rule, or when `store` holds the same
	// purchase with its words already removed. Returns whether it did.
	bool Scrub(NormalizedEvent &ev, const EventStore &store, int64_t nowMs) const;

private:
	struct Removal {
		OAuth::DestinationId dest;
		Chat::Redaction reach;
		int64_t seenMs = 0;
	};
	mutable std::mutex mutex_;
	std::deque<Removal> removals_;
};

class EventHub {
public:
	// Start (or restart) the transport for one connected account. Idempotent per
	// accountId: stops any prior worker for that account first. A no-op when the
	// provider is unknown or makeEvents(acct) returns null.
	void StartAccount(const std::string &accountId, const OAuth::OAuthAccount &acct);

	// Stop + disconnect the transport for one accountId. Idempotent.
	void StopAccount(const std::string &accountId);

	// Drop every stored event of a removed account, and refuse any it still sends: StopAccount
	// only signals its worker, so a poll or a connect already under way can emit after this.
	// Call after the account left the account store. Never waits on admitMutex_ (see Admit),
	// so the UI thread can call it. Returns how many stored events went. A StartAccount for
	// the same account (a reconnect) admits its events again.
	size_t PurgeAccount(const std::string &accountId);

	// One-time startup sweep: StartAccount every connected, scope-current account in
	// the account store. Enforces the always-on/account-lifecycle model (spec §2/§11) so
	// accounts connected in a PRIOR session resume events at boot without a manual
	// reconnect -- the interactive oauth.connect path only covers newly connected
	// accounts. Idempotent via StartAccount (keyed per accountId). Call once after the
	// provider registry + account store are ready.
	void StartConnectedAccounts();

	// Stop + disconnect every transport. Idempotent; called from Bridge::Shutdown.
	void StopAll();

	// Normalize -> dedupe/persist in the store -> alive-guarded emit "events.new".
	// The public entry every transport's emit funnels into, and what the chat layer's
	// YouTube-during-live sink calls directly. Thread-safe (serialized by admitMutex_).
	void Ingest(const NormalizedEvent &ev);

	// A chat moderator's removal on `dest`: remember it for events still to come
	// (RecentRemovals), take the viewer's words off every stored event it reaches
	// (RedactModeratedEvents), then tell the events docks and the overlay widgets which
	// events lost them. Called on the chat transport worker, right after the chat ring's own
	// redaction.
	void ApplyModeration(const OAuth::DestinationId &dest, const Chat::ModerationOp &op);

	// Re-fire the stored event `id` to the overlay widgets that take a replay
	// (OverlayServer::Replay), as stored now. Returns how many widgets got it, once it is
	// sent, or nullopt when no stored event has that id.
	std::optional<size_t> Replay(const std::string &id);

	// Self-tests only: called under admitMutex_ with each events.new and events.redacted
	// payload, as it goes out and in that order. Null to stop.
	void SetFanoutObserver(std::function<void(const char *name, const json &payload)> observer);

private:
	// Store `ev` (stamped with `accountId` when it names none) without the words a removal
	// already took (RecentRemovals::Scrub). On success `ev` is what was stored. False for a
	// duplicate, an event with no id, or one of a purged account. The purge mark is read before
	// the store pass and again after it: a purge that marked the account in between may have
	// purged before this event was stored, so the event takes its account's rows out itself.
	// Caller holds admitMutex_.
	bool Admit(NormalizedEvent &ev, const std::string &accountId);
	// Whether PurgeAccount marked `accountId` since its last StartAccount.
	bool Purged(const std::string &accountId) const;
	// The fan-out observer, when one is set. Caller holds admitMutex_.
	void Observe(const char *name, const json &payload) const;

	// An admission and a removal never interleave. Ingest and the backfill loop hold it from
	// the store pass through the events.new post and the overlay broadcast, Replay across its
	// store read and broadcast, and ApplyModeration from Remember through the events.redacted
	// post and the `eventredaction` broadcast. So an event is either admitted wholly before a
	// removal (stored and sent with its words, then reached by the removal's store pass,
	// whose frames follow its own on each channel) or wholly after (scrubbed by
	// RecentRemovals before it is stored or sent): once a removal's frames are out, none
	// carrying the words it took follows them. The bridge posts are CEF tasks queued in that
	// same order, and so are the overlay frames: the overlay server's one fan-out queue sends
	// them in the order they were queued here (OverlayServer::Enqueue).
	//
	// Lock order: admitMutex_, then RecentRemovals::mutex_, EventStore::mutex_ or
	// EventStore's writer (OrderedStoreSave) (one at a time; each is released before the next is taken), then
	// the overlay's BroadcastTally mutex and its save, then the overlay's fan-out queue mutex,
	// then the log. Nothing reached under it calls back into EventHub: the event store,
	// StorePaths, the overlay server and widget store take no Events::Hub(), and the bridge emit
	// only queues script for the renderer. Every caller outside the self-tests is a worker
	// thread, so the UI thread never waits on it. It is held across the store's disk write, so
	// a slow disk delays the next admission; never across an overlay send, which happens on
	// the fan-out thread (Replay waits for its count after releasing it).
	std::mutex admitMutex_;
	RecentRemovals removals_; // written and read under admitMutex_ by the hub
	std::function<void(const char *, const json &)> fanoutObserver_; // guarded by admitMutex_
	// Accounts purged and not reconnected since. A leaf lock: taken alone, never with another.
	mutable std::mutex purgedMutex_;
	std::unordered_set<std::string> purged_; // guarded by purgedMutex_

	struct Active {
		std::shared_ptr<EventTransport> transport; // hub-owned, shared with the worker
		std::shared_ptr<std::atomic<bool>> stop;
	};

	std::mutex mutex_;
	std::map<std::string, Active> active_; // keyed by accountId
};

// The events in `store` that `op` on `dest` reaches, redacted and persisted (see
// EventStore::RedactMessages): a delete names the event that arrived as that chat message
// (`msgId`) and the REST poll's copy of the same purchase, a ban or timeout every event of
// that viewer (`authorId`), both on that destination only and, when the op carries one, said
// at or before `beforeTs`. An event stored with no broadcast counts as on every destination
// of its account. Every stored event was admitted before the op, so the op's seq bound
// reaches all of them, as it reaches every chat.db row. A clear of the whole chat reaches
// none: it clears the chat, not what viewers paid to say.
std::vector<NormalizedEvent> RedactModeratedEvents(EventStore &store, const OAuth::DestinationId &dest,
						   const Chat::ModerationOp &op);

// Process-wide singletons (function-local statics, mirroring the chat hub + stores).
// Store() is the single EventStore instance events.list/clear and Ingest all hit.
EventHub &Hub();
EventStore &Store();

} // namespace Events

#endif // OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_HUB_HPP_
