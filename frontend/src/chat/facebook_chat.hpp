#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_FACEBOOK_CHAT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_FACEBOOK_CHAT_HPP_

#include <atomic>
#include <mutex>
#include <string>

#include "chat_transport.hpp"

// The Facebook live-comments transport (Phase 9). Facebook pushes one live video's
// comments as server-sent events from streaming-graph.facebook.com/{live-video-id}/
// live_comments, read with the PAGE token that created the broadcast -- so both halves of
// the target come from FacebookProvider, which holds them per destination: the live-video
// id arrives as the hub's `channelRef`, the Page token through chatPageToken.
//
// That stream has no resume. Meta advertises neither a `retry:` interval nor a
// Last-Event-ID on it, so a dropped connection simply loses whatever arrived while it was
// down. Every reconnect therefore gap-fills over the /{live-video-id}/comments edge from
// the newest comment already seen before resuming the push read, and a bounded id memory
// removes the overlap that definitionally creates.
namespace OAuth {
class FacebookProvider;
}

namespace Chat {

class FacebookChat : public ChatTransport {
public:
	explicit FacebookChat(OAuth::FacebookProvider &owner) : owner_(owner) {}

	// Read loop: a gap-fill (on every connection after the first) then one long-lived SSE
	// read, repeated until canceled or refused for good. It owns its own reconnect, the
	// hub calling this exactly once per go-live. `channelRef` is the live-video id; empty
	// (no active broadcast) is a clean no-op that returns false with an empty `err`.
	bool connect(const ChatContext &ctx, OAuth::OAuthAccount &acct, const std::string &channelRef,
		     std::string &err) override;

	// Always refuses. Meta documents the live-video comments edge as read-only, and no
	// other edge posts a comment onto a live video as the Page, so there is no request to
	// attempt.
	bool send(OAuth::OAuthAccount &acct, const std::string &text, std::string &err) override;

	// The read carries every comment on the live video, this Page's own included, so a
	// local echo would double any message that could be sent. Stated as what the READ
	// does, which is the half that stays true whatever becomes of the send path -- and it
	// is what leaves no wrong render reachable either way, since the hub echoes only after
	// a send SUCCEEDS and this one cannot.
	bool reflectsOwnSend() const override { return true; }

	// Flip the stop flag so the read loop returns promptly. The worker that owns the loop
	// performs the teardown: nothing socket-bound is held here, since aborting a streaming
	// read is the onChunk return value rather than a close from this side.
	void disconnect() override { stop_.store(true, std::memory_order_release); }

private:
	OAuth::FacebookProvider &owner_;
	std::mutex runMutex_;           // serializes connect() across overlapping Start/Stop
	std::atomic<bool> stop_{false}; // set by disconnect(); secondary to ctx.canceled()
};

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_FACEBOOK_CHAT_HPP_
