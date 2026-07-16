#ifndef OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_HUB_HPP_
#define OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_HUB_HPP_

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "event_store.hpp"
#include "event_transport.hpp"

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

class EventHub {
public:
	// Start (or restart) the transport for one connected account. Idempotent per
	// accountId: stops any prior worker for that account first. A no-op when the
	// provider is unknown or makeEvents(acct) returns null.
	void StartAccount(const std::string &accountId, const OAuth::OAuthAccount &acct);

	// Stop + disconnect the transport for one accountId. Idempotent.
	void StopAccount(const std::string &accountId);

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
	// YouTube-during-live sink calls directly. Thread-safe (the store locks itself).
	void Ingest(const NormalizedEvent &ev);

private:
	struct Active {
		std::string providerId;                    // for the transport-health id on stop
		std::shared_ptr<EventTransport> transport; // hub-owned, shared with the worker
		std::shared_ptr<std::atomic<bool>> stop;
	};

	std::mutex mutex_;
	std::map<std::string, Active> active_; // keyed by accountId
};

// Process-wide singletons (function-local statics, mirroring the chat hub + stores).
// Store() is the single EventStore instance events.list/clear and Ingest all hit.
EventHub &Hub();
EventStore &Store();

} // namespace Events

#endif // OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_HUB_HPP_
