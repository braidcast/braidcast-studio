#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_KICK_CHAT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_KICK_CHAT_HPP_

#include <atomic>
#include <string>

#include "chat_transport.hpp"

// Kick chat transport (Phase 9.0): reads live chat off Kick's Pusher WebSocket
// and sends via Kick's official REST chat API. Owned by KickProvider and returned
// from KickProvider::chat(); the ChatHub runs connect() on a worker thread between
// go-live and stop.
//
// Read path is REVERSE-ENGINEERED from the Kick web client (Pusher app key /
// cluster, the kick.com/api/v2 chatroom-id lookup, the `[emote:id:name]` markup);
// the send path is the official documented endpoint. See
// docs/superpowers/specs/2026-06-30-phase9-research.md §1 -- every unofficial fact
// is flagged at its use site.
namespace OAuth {
class KickProvider;
}

namespace Chat {

class KickChat : public ChatTransport {
public:
	explicit KickChat(OAuth::KickProvider &provider) : provider_(provider) {}

	// Resolve the chatroom id from the slug, connect the Pusher socket, subscribe,
	// and run the read loop (reconnecting with backoff on a drop) until canceled or
	// disconnect(). Returns false with empty `err` on a clean cancel, false with
	// `err` only on a fatal (no-WebSocket) failure.
	bool connect(const ChatContext &ctx, OAuth::OAuthAccount &acct, const std::string &channelRef,
		     std::string &err) override;

	// Send `text` as `acct` via the official REST chat endpoint (reactive-401 +
	// force refresh handled by KickProvider::SendAuthed).
	bool send(OAuth::OAuthAccount &acct, const std::string &text, std::string &err) override;

	// Signal the read loop to stop. May run on a different thread than connect()'s
	// worker, so it only flips the flag -- the worker owns the WsClient teardown
	// (curl handles are not safe for concurrent use). The worker's recv polls at
	// ~250ms, so the loop returns within the ChatHub's ~0.5s budget.
	void disconnect() override { stop_.store(true, std::memory_order_release); }

private:
	OAuth::KickProvider &provider_;
	std::atomic<bool> stop_{false};
};

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_KICK_CHAT_HPP_
