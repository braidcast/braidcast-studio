#ifndef OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_TRANSPORT_HPP_
#define OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_TRANSPORT_HPP_

#include <functional>
#include <string>
#include <vector>

#include "../oauth/provider.hpp"
#include "event_model.hpp"
#include "transport_health.hpp"

// The per-platform live-event transport interface (Phase 9.2a), the sibling of
// ChatTransport. One concrete transport per platform (Twitch EventSub WS, Kick
// Pusher, YouTube REST) lives in its own file under frontend/src/events/ and is
// constructed per account by that platform's StreamProvider (StreamProvider::makeEvents),
// then owned by the EventHub (as a shared_ptr shared with its worker) for the account's
// live session. The EventHub runs each transport on its own worker on the ACCOUNT-CONNECT
// lifecycle (not go-live, unlike ChatHub), normalizes every source into
// NormalizedEvent, and fans it to the store + JS. A transport never touches the
// hub: the hub passes in the emit/cancel context, so adding a platform is purely
// one new file + the provider's makeEvents() override.
namespace Events {

// The runtime context the hub hands a transport for one live connection. `emit`
// pushes one already-normalized event toward the store + JS (the hub routes it
// through EventHub::Ingest, which dedupes/persists/emits). `canceled` returns true
// once the account disconnects or the bridge tears down; every blocking read/poll
// loop MUST check it frequently and bail promptly.
struct EventContext {
	std::function<void(const NormalizedEvent &)> emit;
	std::function<bool()> canceled;

	// Optional: a transport calls this from connect()/backfill() right before it
	// returns a PERMANENT failure -- one that will never succeed on retry without an
	// explicit re-Start or account change (e.g. the build lacks WebSocket support, or
	// the account has no resolvable broadcaster/channel id). The hub then stops the
	// reconnect loop for this account instead of spinning it forever. A transient
	// failure (dropped socket, network blip) simply does not call this. Always
	// populated by the hub; transports still guard `if (ctx.markFatal)` for safety.
	std::function<void()> markFatal;

	// Report this transport's connection-health transition to the shared aggregator
	// (R14/G1). Always populated by the hub with the transport id bound in; a real-time
	// transport calls it with Connected once its socket is established and handshaked.
	// The Connecting/Reconnecting/Failed/Disconnected lifecycle is reported by the hub,
	// which owns the connect/retry loop. Guard `if (ctx.reportHealth)` for safety.
	std::function<void(Transports::TransportHealth::State state, const std::string &error)> reportHealth;
};

class EventTransport {
public:
	virtual ~EventTransport() = default;

	// Run the always-on real-time source (Twitch EventSub WS / Kick Pusher) on the
	// calling worker thread until ctx.canceled() turns true or the connection drops;
	// emit normalized events via ctx.emit. Returns false with `err` set on a fatal
	// failure; a clean cancel returns false with `err` possibly empty. YouTube's
	// transport may no-op here (its real-time events arrive via the chat sink).
	virtual bool connect(const EventContext &ctx, OAuth::OAuthAccount &acct, std::string &err) = 0;

	// One-shot REST seed on connect (recent followers/superchats/...). Best-effort:
	// append normalized history to `out`; an empty result is fine. Default: no-op.
	virtual bool backfill(const EventContext &ctx, OAuth::OAuthAccount &acct, std::vector<NormalizedEvent> &out,
			      std::string &err)
	{
		(void)ctx;
		(void)acct;
		(void)out;
		(void)err;
		return true;
	}

	// Optional periodic REST tick (YouTube pre-live). The hub calls it on the
	// pollIntervalMs() cadence from the worker between real-time reads. Default no-op.
	virtual void poll(const EventContext &ctx, OAuth::OAuthAccount &acct)
	{
		(void)ctx;
		(void)acct;
	}

	// The desired poll() cadence in milliseconds, or 0 for none (the hub never calls
	// poll). Default: none.
	virtual int pollIntervalMs() { return 0; }

	// Signal any read loop to stop and close open sockets. May be called from a
	// different thread than connect()'s worker, so it must only flip a flag / shut a
	// socket so the worker's loop returns promptly. Default: no-op.
	virtual void disconnect() {}
};

} // namespace Events

#endif // OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_TRANSPORT_HPP_
