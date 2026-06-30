#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_TWITCH_CHAT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_TWITCH_CHAT_HPP_

#include <atomic>
#include <mutex>
#include <string>

#include "chat_transport.hpp"
#include "ws_client.hpp"

// The Twitch chat transport (Phase 9.0): IRC-over-WebSocket against
// wss://irc-ws.chat.twitch.tv:443, read + send over one persistent socket. Owned
// by TwitchProvider and handed to the ChatHub via StreamProvider::chat(). The hub
// runs connect() on a dedicated worker between go-live and stop; send() is invoked
// from a separate hub worker, so every libcurl handle access is serialized by
// wsMutex_ (libcurl easy handles are not safe for concurrent use). connect() is
// itself serialized by runMutex_ so an overlapping re-Start (the hub does not join
// old workers) never drives two read loops over the same socket.
namespace OAuth {

class AuthStrategy;

class TwitchChat : public Chat::ChatTransport {
public:
	explicit TwitchChat(AuthStrategy *auth) : auth_(auth) {}

	bool connect(const Chat::ChatContext &ctx, OAuthAccount &acct, const std::string &channelRef,
		     std::string &err) override;
	bool send(OAuthAccount &acct, const std::string &text, std::string &err) override;
	void disconnect() override;

private:
	// Lock wsMutex_ and write one already-CRLF-terminated IRC line. false if the
	// socket is gone. Never called while wsMutex_ is already held.
	bool sendLine(const std::string &line);

	AuthStrategy *auth_; // the provider's auth strategy, for a reactive token refresh

	std::mutex runMutex_;       // serializes connect() across overlapping Start/Stop
	std::mutex wsMutex_;        // serializes every ws_ access (recv vs send vs connect/close)
	Chat::WsClient ws_;         // the shared libcurl-WS socket (guarded by wsMutex_)
	std::atomic<bool> stopped_{false}; // set by disconnect(); secondary to ctx.canceled()
	std::atomic<bool> ready_{false};   // true between JOIN and drop -- gates send()
	std::string channel_;       // joined channel (lowercased); guarded by wsMutex_
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_TWITCH_CHAT_HPP_
