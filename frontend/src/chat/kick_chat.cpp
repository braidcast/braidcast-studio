#include "kick_chat.hpp"

#include <chrono>

#include "../http_client.hpp"
#include "../log.hpp"
#include "../oauth/kick_provider.hpp"
#include "kick_pusher.hpp"
#include "third_party_emotes.hpp"
#include "ws_client.hpp"

namespace Chat {

namespace {

// Kick emote image CDN. The web client renders 32/64/128px variants; /fullsize
// serves the original upload. (Research §Emote markup and CDN -- community
// verified, URL shape stable since 2024.)
constexpr const char *kEmoteCdnPrefix = "https://files.kick.com/emotes/";
constexpr const char *kEmoteCdnSuffix = "/fullsize";

long long NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

// Split `content` into normalized fragments, mapping Kick's inline emote markup
// `[emote:<id>:<name>]` to emote fragments and everything else to text.
//
// UNCERTAIN markup: the `[emote:id:name]` form is documented by third-party tools,
// not Kick's official API (research §Emote markup and CDN). Parse defensively --
// any malformed token degrades to literal text rather than crashing.
json ParseFragments(const std::string &content)
{
	json frags = json::array();
	const std::string marker = "[emote:";
	size_t pos = 0;

	while (pos < content.size()) {
		const size_t start = content.find(marker, pos);
		if (start == std::string::npos) {
			frags.push_back(json{{"type", "text"}, {"text", content.substr(pos)}});
			break;
		}
		if (start > pos) {
			frags.push_back(json{{"type", "text"}, {"text", content.substr(pos, start - pos)}});
		}

		const size_t idStart = start + marker.size();
		const size_t colon = content.find(':', idStart);
		const size_t close = content.find(']', idStart);
		if (colon == std::string::npos || close == std::string::npos || colon >= close) {
			// Malformed token: emit the stray '[' as text and resume scanning past it.
			frags.push_back(json{{"type", "text"}, {"text", content.substr(start, 1)}});
			pos = start + 1;
			continue;
		}

		const std::string id = content.substr(idStart, colon - idStart);
		const std::string name = content.substr(colon + 1, close - (colon + 1));
		if (id.empty()) {
			frags.push_back(json{{"type", "text"}, {"text", content.substr(start, 1)}});
			pos = start + 1;
			continue;
		}
		frags.push_back(json{{"type", "emote"},
				     {"code", name},
				     {"url", kEmoteCdnPrefix + id + kEmoteCdnSuffix}});
		pos = close + 1;
	}
	return frags;
}

void EmitState(const ChatContext &ctx, bool connected, const std::string &error)
{
	json p = json{{"event", "chat.state"}, {"platform", "kick"}, {"connected", connected}};
	if (!error.empty()) {
		p["error"] = error;
	}
	ctx.emit(p);
}

// Decode one App\Events\ChatMessageEvent Pusher frame and emit a normalized
// chat.message. Defensive throughout: a double-decode failure or any missing
// required field drops the message rather than throwing (research flags the whole
// Kick payload as reverse-engineered).
void HandleChatMessage(const json &outer, const ChatContext &ctx, const std::string &chatroomId,
		       const std::unordered_map<std::string, std::string> &thirdPartyEmotes)
{
	// Pusher's `data` is itself a JSON-ENCODED STRING (double-encoded); decode again.
	auto dataIt = outer.find("data");
	if (dataIt == outer.end() || !dataIt->is_string()) {
		return;
	}
	const json inner = json::parse(dataIt->get<std::string>(), nullptr, false);
	if (!inner.is_object()) {
		HostLog("[chat] kick: dropped message (data double-decode failed)");
		return;
	}

	std::string name;
	std::string color;
	json badges = json::array();
	auto senderIt = inner.find("sender");
	if (senderIt != inner.end() && senderIt->is_object()) {
		const json &sender = *senderIt;
		name = sender.value("username", std::string());
		if (name.empty()) {
			name = sender.value("slug", std::string());
		}
		auto idIt = sender.find("identity");
		if (idIt != sender.end() && idIt->is_object()) {
			color = idIt->value("color", std::string());
			auto bIt = idIt->find("badges");
			if (bIt != idIt->end() && bIt->is_array()) {
				for (const json &b : *bIt) {
					if (!b.is_object()) {
						continue;
					}
					const std::string kind = b.value("type", std::string());
					if (kind.empty()) {
						continue;
					}
					// Kick identity badges carry no image URL; url is optional
					// in the normalized model, so omit it.
					badges.push_back(json{{"kind", kind}});
				}
			}
		}
	}
	if (name.empty()) {
		return; // no resolvable author -> skip
	}

	std::string msgId;
	auto msgIdIt = inner.find("id");
	if (msgIdIt != inner.end()) {
		if (msgIdIt->is_string()) {
			msgId = msgIdIt->get<std::string>();
		} else if (msgIdIt->is_number_integer()) {
			msgId = std::to_string(msgIdIt->get<long long>());
		}
	}

	const std::string content = inner.value("content", std::string());

	ctx.emit(json{
		{"event", "chat.message"},
		{"platform", "kick"},
		{"channelId", chatroomId},
		{"id", msgId},
		// Receipt time, not the payload's created_at (ISO-8601 parsing skipped); the
		// two differ only by network latency.
		{"ts", NowMs()},
		{"author", json{{"name", name}, {"color", color}, {"badges", badges}}},
		// Native [emote:...] fragments win: ApplyThirdPartyEmotes only rewrites the
		// plain-text runs ParseFragments emits, leaving Kick emote fragments verbatim.
		{"fragments", ApplyThirdPartyEmotes(ParseFragments(content), thirdPartyEmotes)},
	});
}

} // namespace

bool KickChat::connect(const ChatContext &ctx, OAuth::OAuthAccount &acct, const std::string &channelRef,
		       std::string &err)
{
	(void)acct; // read path is public/unauthenticated; tokens only matter on send()
	stop_.store(false, std::memory_order_release);

	if (!WsClient::WebSocketsSupported()) {
		err = "libcurl lacks WebSocket support; Kick chat unavailable";
		return false; // fatal: logged by the hub
	}

	const std::string pusherUrl = KickPusherUrl();

	const auto canceled = [&] { return stop_.load(std::memory_order_acquire) || ctx.canceled(); };

	std::string chatroomId; // resolved once, cached for the session
	// Third-party (7TV/BTTV) emote map, local to this worker: built once after the
	// channel id resolves and read only by this worker's read loop. A member would
	// race a previous generation's still-running worker (ChatHub::Start does not join
	// the old worker and the provider returns the same KickChat instance).
	std::unordered_map<std::string, std::string> thirdPartyEmotes;
	Backoff backoff;

	while (!canceled()) {
		// Resolve the chatroom id on first attempt; reuse the cached value across
		// reconnects (a channel's chatroom id is immutable, so a re-fetch per the
		// research's reconnect note adds latency for no benefit). Bounded timeout so
		// this blocking fetch cannot stall teardown.
		if (chatroomId.empty()) {
			std::string lookupErr;
			std::string channelId; // numeric channel id; 7TV/BTTV key kick sets by it
			if (!ResolveKickChannelIds(channelRef, chatroomId, channelId, lookupErr)) {
				EmitState(ctx, false, lookupErr);
				if (CancelableSleep(backoff.next(), canceled)) {
					break;
				}
				continue;
			}
			// Build the third-party emote map once, on this worker, from the resolved
			// channel id. Best-effort + cancel-polled; a failure just yields fewer
			// emotes. Cached across reconnects since chatroomId now stays non-empty.
			thirdPartyEmotes = FetchThirdPartyEmotes(EmotePlatform::Kick, "", channelId, canceled);
		}

		WsClient ws;
		std::string wsErr;
		if (!ws.connect(pusherUrl, wsErr)) {
			EmitState(ctx, false, wsErr);
			if (CancelableSleep(backoff.next(), canceled)) {
				break;
			}
			continue;
		}

		// Read loop for this socket. Returns when the socket drops/errors or the
		// transport is canceled; the outer loop then reconnects (with backoff) unless
		// canceled.
		std::string dropErr;
		while (!canceled()) {
			std::string frame;
			bool isText = false;
			std::string recvErr;
			if (!ws.recv(frame, isText, recvErr)) {
				dropErr = recvErr;
				break;
			}
			if (frame.empty() || !isText) {
				continue; // poll timeout, auto-PONGed ping, or partial chunk
			}

			const json outer = json::parse(frame, nullptr, false);
			if (!outer.is_object()) {
				continue;
			}
			const std::string event = outer.value("event", std::string());

			if (event == "pusher:connection_established") {
				// Public channel -> empty auth (research §Subscribe frame).
				ws.sendText(json{{"event", "pusher:subscribe"},
						 {"data", json{{"auth", ""},
							       {"channel", "chatrooms." + chatroomId + ".v2"}}}}
						    .dump());
			} else if (event == "pusher_internal:subscription_succeeded") {
				backoff.reset();
				EmitState(ctx, true, "");
			} else if (event == "pusher:ping") {
				// App-level Pusher ping (distinct from the WS-level ping WsClient
				// auto-PONGs); reply with an app-level pong.
				ws.sendText("{\"event\":\"pusher:pong\",\"data\":{}}");
			} else if (event == "App\\Events\\ChatMessageEvent") {
				HandleChatMessage(outer, ctx, chatroomId, thirdPartyEmotes);
			} else if (event == "pusher:error") {
				HostLog("[chat] kick pusher error: " + frame);
			}
			// All other events (subscriptions, reactions, gifts, ...) are ignored.
		}

		ws.close();
		if (canceled()) {
			break;
		}
		EmitState(ctx, false, dropErr.empty() ? "connection lost" : dropErr);
		if (CancelableSleep(backoff.next(), canceled)) {
			break;
		}
	}

	return false; // clean cancel: false with empty err (the hub suppresses the log)
}

bool KickChat::send(OAuth::OAuthAccount &acct, const std::string &text, std::string &err)
{
	// Official documented endpoint (research §SEND path, verified against
	// docs.kick.com): POST /public/v1/chat with a chat:write Bearer token. Pure REST
	// -- no Pusher socket involved.
	json body = json::object();
	body["content"] = text;
	body["type"] = "user"; // send as the authenticated user (the broadcaster)

	// broadcaster_user_id is required for type="user" and is an integer on the wire.
	int uid = 0;
	try {
		uid = std::stoi(acct.userId);
	} catch (const std::exception &) {
		uid = 0;
	}
	if (uid <= 0) {
		// Identity not resolved yet -> fetch it (also persists login/userId).
		if (!provider_.fetchIdentity(acct, err)) {
			return false;
		}
		try {
			uid = std::stoi(acct.userId);
		} catch (const std::exception &) {
			uid = 0;
		}
	}
	if (uid <= 0) {
		err = "Kick chat send: could not resolve broadcaster user id";
		return false;
	}
	body["broadcaster_user_id"] = uid;

	Http::HttpReq req;
	req.method = "POST";
	req.url = "https://api.kick.com/public/v1/chat";
	req.contentType = "application/json";
	req.body = body.dump();

	// SendAuthed does the proactive refresh + reactive-401 force-refresh + one retry
	// (Kick rotates refresh tokens; ensureFresh(force=true) re-reads/writes the
	// rotated token store-coherently).
	Http::HttpResponse resp;
	if (!provider_.SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "Kick chat send failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}
	return true;
}

} // namespace Chat
