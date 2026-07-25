#ifndef OBS_MULTISTREAM_FRONTEND_EVENTS_TRANSPORT_HEALTH_HPP_
#define OBS_MULTISTREAM_FRONTEND_EVENTS_TRANSPORT_HEALTH_HPP_

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../oauth/provider.hpp" // OAuth::DestinationId, OAuth::DestinationKey

// The cross-cutting transport-health aggregator (Phase 9.x, R14/G1): one shared
// observable surface for "is this transport connected / reconnecting / failed",
// fed by the chat, events, and overlay transports and consumed by the bridge as a
// snapshot method (transports.health) plus an emit-on-change event
// (transports.healthChanged). It mirrors the multistream status pattern exactly --
// MultistreamEngine holds the per-output state and NotifyChanged() -> onStatusChanged
// -> Bridge::EmitMultistreamChanged; this holds the per-transport state and
// NotifyChanged() -> onChanged -> Bridge::EmitTransportsHealthChanged -- so the two
// health surfaces share one shape, not two parallel mechanisms.
//
// Lifetime: a function-local-static singleton (Transports::Health()), like the chat
// and event hubs it serves -- their detached workers report from their own threads
// and outlive a hub Stop(), so the aggregator they call must outlive them too. The
// bridge wires onChanged in Bridge::Init and clears it (+ Clear()) in Bridge::Shutdown.
namespace Transports {

class TransportHealth {
public:
	// Connection lifecycle of one transport. Carried on the wire as its lowercase
	// StateName; the web transportHealthStore maps it to a badge. Single source of
	// truth for the contract (mirrors MultistreamEngine::State / StateName).
	enum class State { Connecting, Connected, Reconnecting, Failed, Disconnected };

	// Lowercase state name for the status JSON. Single source of truth.
	static const char *StateName(State state);

	// One transport's current health row.
	struct Entry {
		std::string id; // stable transport id (see ChatTransportId / EventsTransportId below)
		State state = State::Disconnected;
		std::string lastError; // set for Reconnecting/Failed (the drop/failure reason)
		int64_t updatedAtMs = 0;
	};

	// Fired (OUTSIDE the mutex) after any state change, so the bridge can push
	// transports.healthChanged. Wired once by Bridge::Init; nulled in Bridge::Shutdown.
	std::function<void()> onChanged;

	// Record a transport's current state. Coalesces: a report identical to the stored
	// (state, error) is a no-op (no notify). Thread-safe; called from transport worker
	// threads. The notify runs after the lock is released so onChanged (which reads
	// Snapshot() back on the UI thread) never re-enters the held mutex.
	void Report(const std::string &id, State state, const std::string &error = "");

	// All rows, id-sorted for stable output. The snapshot source for transports.health
	// and the transports.healthChanged payload.
	std::vector<Entry> Snapshot() const;

	// Drop every row (a fresh session starts clean). Called during teardown.
	void Clear();

private:
	void NotifyChanged();

	mutable std::mutex mutex_;
	std::map<std::string, Entry> entries_;
};

// Process-wide aggregator accessor (function-local static, mirroring Chat::Hub() /
// Events::Hub()).
TransportHealth &Health();

// Stable transport-id conventions, so the id scheme lives in one place (a hub worker
// binds the report, a hub Stop() reports the matching Disconnected -- both must agree).
//
// Keyed by DESTINATION, never by platform: chat runs one transport per live destination, so
// an account streaming two orientations has two independent healths. Sharing a platform-wide
// id makes them overwrite each other last-writer-wins, which lets a dead chat read as healthy
// off a row actually holding its sibling's state. The tail is OAuth::DestinationKey rather
// than a second format, so a health row matches the key every other destination-scoped
// surface uses (viewers.changed perDestination[].key). An events transport runs per ACCOUNT,
// so it reports under that account's account-wide destination -- key = the bare accountId.
inline std::string ChatTransportId(const OAuth::DestinationId &dest)
{
	return "chat:" + OAuth::DestinationKey(dest);
}
inline std::string EventsTransportId(const OAuth::DestinationId &dest)
{
	return "events:" + OAuth::DestinationKey(dest);
}
inline constexpr const char *kOverlayTransportId = "overlay";

} // namespace Transports

#endif // OBS_MULTISTREAM_FRONTEND_EVENTS_TRANSPORT_HEALTH_HPP_
