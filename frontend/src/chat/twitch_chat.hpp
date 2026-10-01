#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_TWITCH_CHAT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_TWITCH_CHAT_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "chat_archive.hpp" // Chat::ModerationOp
#include "chat_transport.hpp"
#include "third_party_emotes.hpp"
#include "ws_client.hpp"

// The Twitch chat transport (Phase 9.0): IRC-over-WebSocket against
// wss://irc-ws.chat.twitch.tv:443, read + send over one persistent socket.
// Constructed per account by TwitchProvider::makeChat and owned by the ChatHub. The hub
// runs connect() on a dedicated worker between go-live and stop; send() is invoked
// from a separate hub worker, so every libcurl handle access is serialized by
// wsMutex_ (libcurl easy handles are not safe for concurrent use). connect() is
// itself serialized by runMutex_ so an overlapping re-Start (the hub does not join
// old workers) never drives two read loops over the same socket.
namespace OAuth {

class AuthStrategy;

// Normalize one raw IRC line (tags included, no CRLF) into a chat.message frame, or a null
// json when the line is not a chat line. Pure apart from the "now" fallback for a line with
// no `tmi-sent-ts`; the read loop runs the same parse + normalization, so the offline
// self-test exercises exactly what live chat does.
Chat::json NormalizeTwitchChatLine(const std::string &line, const std::string &channel,
				   const Chat::ThirdPartyEmoteMap &emotes);

// Read one raw IRC line as a moderator's removal, or nothing when it is not one. CLEARMSG
// deletes the message its `target-msg-id` names, which is that line's `id` tag and so its
// frame's `id`. CLEARCHAT with `target-user-id` removes that user's messages, a ban and a
// timeout alike, matching the frames' `author.id` (their `user-id` tag); a bare CLEARCHAT
// clears the whole chat. The op's `dest` is left empty for the hub to fill in.
std::optional<Chat::ModerationOp> ParseTwitchModerationLine(const std::string &line);

// The streamer's sent messages whose local echo waits for the platform id. Twitch answers
// our PRIVMSG with a USERSTATE whose `id` tag is the sent message's id, the id a later
// CLEARMSG names. Several sends can be in flight, and Twitch drops a message it rejects
// (rate limit, follower-only) with no USERSTATE, so a USERSTATE is matched to its PRIVMSG by
// the `client-nonce` tag the PRIVMSG carried and Twitch hands back, never by arrival order.
// An echo no USERSTATE claims within kEchoIdWaitMs goes out without an id, as does every
// echo still held when the socket drops. An echo is staged before its PRIVMSG is written
// but goes out only once the write succeeded (MarkSent) or a USERSTATE proves Twitch got
// it, so a failed send is never shown. A clear read while echoes are held leaves them: the
// socket carries Twitch's replies in the order Twitch acted, so a message it accepted before
// the clear has had its USERSTATE read, and its echo admitted, before the CLEARCHAT, where
// the clear's `before` bound reaches it. A held echo was accepted after the clear, or not
// yet. When a connection answers a sent PRIVMSG without the nonce, the queue stops holding
// echoes until the next connection.
// Thread-safe: the send worker stages, marks sent and drops; the read worker does the rest.
class TwitchEchoQueue {
public:
	// How long a sent echo waits for its USERSTATE. A round trip on one socket takes well
	// under a second; a rejected message never gets one.
	static constexpr int64_t kEchoIdWaitMs = 2000;
	// Echoes waiting at once. A send past it is echoed by the hub straight away, with no id.
	static constexpr size_t kMaxPending = 32;

	// Hold `echo` while its PRIVMSG, tagged with `nonce`, is written. On success `echo` is
	// moved from; false (queue full, or this connection returns no nonce) leaves it with
	// the caller.
	bool Stage(const std::string &nonce, Chat::json &echo);
	// The PRIVMSG under `nonce` was written: its echo now waits for its USERSTATE until
	// `deadlineMs`. Returns the echo when it must go out now instead, because this
	// connection was found to return no nonce while the write was in flight.
	std::optional<Chat::json> MarkSent(const std::string &nonce, int64_t deadlineMs);
	// Forget a staged echo whose PRIVMSG never went out.
	void Drop(const std::string &nonce);
	// The echo staged under `nonce`, keyed by `id` (or left without one when `id` is
	// empty); nullopt when nothing is staged under it. A USERSTATE proves the write, so
	// this takes an echo whose MarkSent has not come yet too.
	std::optional<Chat::json> Resolve(const std::string &nonce, const std::string &id);
	// A USERSTATE answered a sent PRIVMSG without the nonce: hold no more echoes on this
	// connection, and return every sent echo held, oldest first, unkeyed.
	std::vector<Chat::json> NonceMissing();
	// A new connection, which may return the nonce again.
	void NewConnection();
	// The sent echoes past their deadline at `nowMs`, oldest first, unkeyed.
	std::vector<Chat::json> TakeExpired(int64_t nowMs);
	// Every sent echo, oldest first, unkeyed: the socket they were sent on is gone. An
	// echo whose write has not finished stays with its send worker.
	std::vector<Chat::json> TakeSent();

private:
	struct Pending {
		std::string nonce;
		Chat::json echo;
		bool sent = false;
		int64_t deadlineMs = 0; // meaningful once sent
	};
	// Remove and return the sent entries `take` picks, oldest first. Caller holds mutex_.
	template<typename Pick> std::vector<Chat::json> TakeSentLocked(Pick take);

	std::mutex mutex_;
	std::deque<Pending> pending_;
	bool nonceMissing_ = false; // this connection answered a PRIVMSG without the nonce
};

// What a raw IRC line says about our sent PRIVMSGs: nothing (not a USERSTATE, or the one
// Twitch sends on JOIN, which carries no `id`), or a USERSTATE answering a sent message,
// with or without the `client-nonce` the PRIVMSG carried.
enum class EchoAnswer { None, WithNonce, WithoutNonce };
EchoAnswer ReadTwitchEchoAnswer(const std::string &line);

// Read one raw IRC line as the USERSTATE that answers a staged echo's PRIVMSG: the echoes it
// releases. With its nonce, that one echo keyed by the sent message's id; without it, every
// sent echo held, unkeyed (NonceMissing). Empty for any other line, a JOIN USERSTATE, or a
// nonce naming nothing staged. The read loop runs the same match, so the offline self-test
// exercises exactly what live chat does.
std::vector<Chat::json> ClaimTwitchEcho(const std::string &line, TwitchEchoQueue &echoes);

class TwitchChat : public Chat::ChatTransport {
public:
	explicit TwitchChat(AuthStrategy *auth) : auth_(auth) {}

	bool connect(const Chat::ChatContext &ctx, OAuthAccount &acct, const std::string &channelRef,
		     std::string &err) override;
	bool send(OAuthAccount &acct, const std::string &text, std::string &err) override;
	// Takes the echo (TwitchEchoQueue) and tags the PRIVMSG with its nonce, so the read
	// loop emits the echo keyed by the id Twitch gives the message.
	bool sendEchoed(OAuthAccount &acct, const std::string &text, Chat::json &echo, std::string &err) override;
	void disconnect() override;

	// Twitch IRC never reflects the sender's own PRIVMSG back over the socket, so
	// the base's reflectsOwnSend()=false stands and the hub echoes sends locally,
	// grouped under the joined channel.
	std::string channelId() const override
	{
		std::lock_guard<std::mutex> lock(wsMutex_);
		return channel_;
	}

private:
	// Lock wsMutex_ and write one already-CRLF-terminated IRC line. false if the
	// socket is gone. Never called while wsMutex_ is already held.
	bool sendLine(const std::string &line);
	// Send `text` to the joined channel as one PRIVMSG, tagged with `nonce` when it is not
	// empty. false + `err` when not connected or the write failed.
	bool sendPrivmsg(const std::string &text, const std::string &nonce, std::string &err);
	// Emit every sent echo held through `ctx`: the socket they were sent on is gone, so no
	// USERSTATE will name them. Read worker only.
	void releaseEchoes(const Chat::ChatContext &ctx);

	AuthStrategy *auth_; // the provider's auth strategy, for a reactive token refresh

	std::mutex runMutex_;              // serializes connect() across overlapping Start/Stop
	mutable std::mutex wsMutex_;       // serializes every ws_ access (recv vs send vs connect/close)
	Chat::WsClient ws_;                // the shared libcurl-WS socket (guarded by wsMutex_)
	std::atomic<bool> stopped_{false}; // set by disconnect(); secondary to ctx.canceled()
	std::atomic<bool> ready_{false};   // true between JOIN and drop -- gates send()
	std::string channel_;              // joined channel (lowercased); guarded by wsMutex_
	TwitchEchoQueue echoes_;           // sent messages awaiting their USERSTATE (self-locking)

	// Third-party (7TV/BTTV/FFZ) emote code -> image URL, built once at the top of
	// connect() and only READ by the read loop on that same worker thread, so it
	// needs no lock. send() runs on a different worker but never touches it.
	Chat::ThirdPartyEmoteMap thirdPartyEmotes_;
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_TWITCH_CHAT_HPP_
