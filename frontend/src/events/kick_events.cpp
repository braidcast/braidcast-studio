#include "kick_events.hpp"

#include <array>
#include <chrono>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "../chat/kick_pusher.hpp"
#include "../chat/ws_client.hpp"
#include "../log.hpp"

// !!! REVERSE-ENGINEERED + BEST-EFFORT (see kick_events.hpp) !!!
// The Pusher channel names and `App\Events\...` event names + payload field names
// below are community-reverse-engineered (primary source: Bukk94/KickLib's
// KickClient channel/event mapping, cross-checked against the /api/v2 channel
// lookup) and can DRIFT WITHOUT NOTICE. Every parse is defensive: an unknown event
// name or a missing/mis-typed field drops that one event rather than throwing.

namespace Events {

using json = nlohmann::json;

namespace {

int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

// --- Tolerant field accessors (an unofficial payload may omit or re-type anything).
std::string Str(const json &j, const char *key)
{
	if (!j.is_object()) {
		return std::string();
	}
	auto it = j.find(key);
	if (it == j.end() || !it->is_string()) {
		return std::string();
	}
	return it->get<std::string>();
}

int64_t Num(const json &j, const char *key)
{
	if (!j.is_object()) {
		return 0;
	}
	auto it = j.find(key);
	return (it != j.end() && it->is_number()) ? it->get<int64_t>() : 0;
}

bool Bool(const json &j, const char *key)
{
	if (!j.is_object()) {
		return false;
	}
	auto it = j.find(key);
	return it != j.end() && it->is_boolean() && it->get<bool>();
}

const json &Obj(const json &j, const char *key)
{
	static const json kNull = json(nullptr);
	if (!j.is_object()) {
		return kNull;
	}
	auto it = j.find(key);
	return it == j.end() ? kNull : *it;
}

// Pusher wraps each event's payload in a `data` field that is itself a JSON-ENCODED
// STRING (double-encoded); decode it again. Some client libraries observe `data`
// already as an object, so accept that too. Returns a null json on any failure.
json InnerData(const json &outer)
{
	auto it = outer.find("data");
	if (it == outer.end()) {
		return json(nullptr);
	}
	if (it->is_string()) {
		return json::parse(it->get<std::string>(), nullptr, false);
	}
	if (it->is_object()) {
		return *it;
	}
	return json(nullptr);
}

// Normalize one reverse-engineered Kick Pusher event into `ev`. Returns false for an
// unknown/ignored event (including chat messages, bans, pins, reactions, and the
// count-only follower ping) or one missing its required actor. Best-effort: names +
// shapes are unofficial and validated against KickLib.
//
// Event -> Pusher channel it arrives on (per KickLib):
//   App\Events\SubscriptionEvent        chatrooms.<chatroomId>.v2  {username, months}
//   App\Events\GiftedSubscriptionsEvent chatrooms.<chatroomId>.v2  {gifter_username, gifted_usernames[]}
//   App\Events\StreamHostEvent          chatrooms.<chatroomId>.v2  {host_username, number_viewers}
//   App\Events\StreamHostedEvent        chatrooms.<chatroomId>.v2  {user:{username}, message:{id,numberOfViewers}}
//   App\Events\FollowersUpdated         channel.<channelId>        {username, followed, followersCount, created_at}
bool Normalize(const std::string &event, const json &outer, NormalizedEvent &ev)
{
	const json d = InnerData(outer);
	if (!d.is_object()) {
		return false;
	}

	ev.platform = "kick";
	ev.ts = NowMs(); // Kick's payloads carry no reliable event timestamp -> receipt time

	// Kick emits some events under both the "App\Events\<Name>" and bare "<Name>"
	// forms depending on channel/version (KickLib binds both) -- accept either.
	if (event == "App\\Events\\SubscriptionEvent" || event == "SubscriptionEvent") {
		ev.type = "sub";
		ev.actorName = Str(d, "username");
		if (ev.actorName.empty()) {
			return false;
		}
		ev.months = static_cast<int>(Num(d, "months")); // cumulative; omitted from JSON when 0
		ev.id = "kick:sub:" + ev.actorName + ":" + std::to_string(ev.ts);
		return true;
	}

	if (event == "App\\Events\\GiftedSubscriptionsEvent" || event == "GiftedSubscriptionsEvent") {
		ev.type = "subgift";
		const std::string gifter = Str(d, "gifter_username");
		ev.actorName = gifter.empty() ? "Anonymous" : gifter; // Kick omits the name for anonymous gifts
		int count = 0;
		const json &names = Obj(d, "gifted_usernames");
		if (names.is_array()) {
			count = static_cast<int>(names.size());
		}
		ev.count = count;
		ev.id = "kick:subgift:" + ev.actorName + ":" + std::to_string(count) + ":" + std::to_string(ev.ts);
		return true;
	}

	if (event == "App\\Events\\StreamHostEvent") {
		ev.type = "raid";
		ev.actorName = Str(d, "host_username");
		if (ev.actorName.empty()) {
			return false;
		}
		ev.amount = Num(d, "number_viewers");
		ev.id = "kick:raid:" + ev.actorName + ":" + std::to_string(ev.ts);
		return true;
	}

	if (event == "App\\Events\\StreamHostedEvent") {
		// The nested-shape variant of a host: user.username + message.numberOfViewers,
		// with a real message id we prefer for a stable dedupe key.
		const json &user = Obj(d, "user");
		const json &msg = Obj(d, "message");
		ev.type = "raid";
		ev.actorName = Str(user, "username");
		if (ev.actorName.empty()) {
			return false;
		}
		ev.amount = Num(msg, "numberOfViewers");
		const std::string mid = Str(msg, "id");
		ev.id = mid.empty() ? ("kick:raid:" + ev.actorName + ":" + std::to_string(ev.ts)) : ("kick:raid:" + mid);
		return true;
	}

	if (event == "App\\Events\\FollowersUpdated") {
		// LIMITATION: this event is primarily a followers-COUNT broadcast and fires for
		// unfollows too; the per-follower name is usually absent. Emit a follow ONLY when
		// Kick actually gives us a name AND followed==true, so a nameless "someone
		// followed" never becomes noise (the common count-only case is dropped here).
		const std::string username = Str(d, "username");
		if (username.empty() || !Bool(d, "followed")) {
			return false;
		}
		ev.type = "follow";
		ev.actorName = username;
		// created_at is an integer tick count (not a date) -> a stable per-follow suffix
		// so a redelivery (e.g. on both channel.<id> and channel_<id>) dedupes.
		const int64_t createdAt = Num(d, "created_at");
		ev.id = "kick:follow:" + username + ":" +
			(createdAt != 0 ? std::to_string(createdAt) : std::to_string(ev.ts));
		return true;
	}

	return false; // ChatMessageEvent, bans, pins, reactions, count-only follower pings, ...
}

} // namespace

bool KickEvents::connect(const EventContext &ctx, OAuth::OAuthAccount &acct, std::string &err)
{
	// Serialize against an overlapping re-Start: the EventHub does not join old workers,
	// so a prior connect() might still be unwinding -- block until it releases the socket.
	std::lock_guard<std::mutex> run(runMutex_);
	stopped_.store(false);

	const auto canceled = [&] { return stopped_.load() || (ctx.canceled && ctx.canceled()); };

	if (!Chat::WsClient::WebSocketsSupported()) {
		err = "libcurl lacks WebSocket support; Kick events unavailable";
		return false; // fatal: logged by the hub
	}

	const std::string slug = acct.login; // the Kick channel slug (= account login)
	if (slug.empty()) {
		err = "Kick events: channel slug unavailable; reconnect the account";
		return false;
	}
	const std::string url = Chat::KickPusherUrl();

	std::string chatroomId; // resolved once, cached across reconnects (both immutable)
	std::string channelId;
	Chat::Backoff backoff;

	while (!canceled()) {
		if (chatroomId.empty()) {
			std::string lookupErr;
			if (!Chat::ResolveKickChannelIds(slug, chatroomId, channelId, lookupErr)) {
				HostLog("[events] kick: id lookup failed: " + lookupErr);
				if (Chat::CancelableSleep(backoff.next(), canceled)) {
					break;
				}
				continue;
			}
		}

		{
			std::lock_guard<std::mutex> lock(wsMutex_);
			std::string cerr;
			if (!ws_.connect(url, cerr)) {
				err = cerr;
			}
		}
		if (!ws_.connected()) {
			if (Chat::CancelableSleep(backoff.next(), canceled)) {
				break;
			}
			continue;
		}

		std::string dropErr;
		while (!canceled()) {
			std::string frame;
			bool isText = false;
			std::string recvErr;
			bool ok;
			{
				std::lock_guard<std::mutex> lock(wsMutex_);
				ok = ws_.recv(frame, isText, recvErr);
			}
			if (!ok) {
				dropErr = recvErr;
				break;
			}
			if (frame.empty() || !isText) {
				continue; // poll timeout, auto-PONGed WS ping, or partial chunk
			}

			const json outer = json::parse(frame, nullptr, false);
			if (!outer.is_object()) {
				continue;
			}
			const std::string event = outer.value("event", std::string());

			if (event == "pusher:connection_established") {
				// Subscribe to the chatroom channel (sub/gift/host) and both channel
				// formats (followers). All are public -> empty auth. channel. and
				// channel_ are alternate spellings KickLib binds both of; a duplicate
				// delivery dedupes by event id.
				const std::array<std::string, 3> channels = {
					"chatrooms." + chatroomId + ".v2",
					channelId.empty() ? std::string() : "channel." + channelId,
					channelId.empty() ? std::string() : "channel_" + channelId,
				};
				std::lock_guard<std::mutex> lock(wsMutex_);
				for (const std::string &ch : channels) {
					if (ch.empty()) {
						continue;
					}
					ws_.sendText(json{{"event", "pusher:subscribe"},
							  {"data", json{{"auth", ""}, {"channel", ch}}}}
							     .dump());
				}
			} else if (event == "pusher_internal:subscription_succeeded") {
				backoff.reset(); // a successful subscribe proves a healthy connection
			} else if (event == "pusher:ping") {
				// App-level Pusher ping (distinct from the WS-level ping WsClient
				// auto-PONGs); reply with an app-level pong.
				std::lock_guard<std::mutex> lock(wsMutex_);
				ws_.sendText("{\"event\":\"pusher:pong\",\"data\":{}}");
			} else if (event == "pusher:error") {
				HostLog("[events] kick pusher error: " + frame);
			} else {
				NormalizedEvent ev;
				if (Normalize(event, outer, ev)) {
					ctx.emit(ev);
				}
				// Unknown event names (chat messages, bans, ...) are ignored.
			}
		}

		{
			std::lock_guard<std::mutex> lock(wsMutex_);
			ws_.close();
		}
		if (canceled()) {
			break;
		}
		HostLog(std::string("[events] kick: connection lost") + (dropErr.empty() ? "" : (": " + dropErr)));
		if (Chat::CancelableSleep(backoff.next(), canceled)) {
			break;
		}
	}

	{
		std::lock_guard<std::mutex> lock(wsMutex_);
		ws_.close();
	}
	if (canceled()) {
		err.clear(); // clean cancel: the hub suppresses the log
	}
	return false;
}

void KickEvents::disconnect()
{
	// Only flip the flag: the worker that owns ws_ tears the socket down on its own
	// thread. recv()'s ~250ms poll makes the loop notice promptly.
	stopped_.store(true);
}

} // namespace Events
