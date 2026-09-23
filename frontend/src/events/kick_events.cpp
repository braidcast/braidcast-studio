#include "kick_events.hpp"
#include "../event_names.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <functional>

#include <nlohmann/json.hpp>

#include <util/platform.h> // os_gettime_ns

#include "util/async_task.hpp"
#include "../bridge.hpp"
#include "../chat/kick_pusher.hpp"
#include "../chat/ws_client.hpp"
#include "util/json_util.hpp"
#include "../log.hpp"
#include "../oauth/account_store.hpp"
#include "../oauth/provider.hpp"
#include "util/time_util.hpp"

// !!! REVERSE-ENGINEERED + BEST-EFFORT (see kick_events.hpp) !!!
// The Pusher channel names and `App\Events\...` event names + payload field names
// below are community-reverse-engineered (primary source: Bukk94/KickLib's
// KickClient channel/event mapping, cross-checked against the /api/v2 channel
// lookup) and can DRIFT WITHOUT NOTICE. Every parse is defensive: an unknown event
// name or a missing/mis-typed field drops that one event rather than throwing.

namespace Events {

using json = nlohmann::json;

namespace {

// --- Tolerant field accessors (an unofficial payload may omit or re-type anything).
using JsonUtil::Bool;
using JsonUtil::NumLoose;
using JsonUtil::Obj;
using JsonUtil::Str;
using TimeUtil::NowMs;

// Dispatched on twice -- once to feed the live follower total into channels.stats, once to
// normalize the follow itself -- and the two must not drift apart.
constexpr const char *kFollowersUpdated = "App\\Events\\FollowersUpdated";

// "#RRGGBB" exactly: the only colour shape the dock and overlays paint an actor with.
bool IsHexRgb(const std::string &s)
{
	if (s.size() != 7 || s[0] != '#') {
		return false;
	}
	for (size_t i = 1; i < s.size(); ++i) {
		if (!std::isxdigit(static_cast<unsigned char>(s[i]))) {
			return false;
		}
	}
	return true;
}

// A Kick sender's name colour. The gift payload carries it as `username_color`; KickLib
// models it as `identity.color` (the chat-message shape), so either is accepted.
std::string KickSenderColor(const json &sender)
{
	std::string color = Str(sender, "username_color");
	if (color.empty()) {
		color = Str(Obj(sender, "identity"), "color");
	}
	return IsHexRgb(color) ? color : std::string();
}

} // namespace

// Event -> Pusher channel it arrives on (per KickLib; KicksGifted per KickLib + social_stream):
//   App\Events\SubscriptionEvent        chatrooms.<chatroomId>.v2  {username, months}
//   App\Events\GiftedSubscriptionsEvent chatrooms.<chatroomId>.v2  {gifter_username, gifted_usernames[]}
//   App\Events\StreamHostEvent          chatrooms.<chatroomId>.v2  {host_username, number_viewers}
//   App\Events\StreamHostedEvent        chatrooms.<chatroomId>.v2  {user:{username}, message:{id,numberOfViewers}}
//   App\Events\FollowersUpdated         channel.<channelId>        {username, followed, followersCount, created_at}
//   KicksGifted                         channel.<channelId>        {gift_transaction_id, message,
//                                                                   sender:{username, username_color},
//                                                                   gift:{gift_id, name, amount}}
bool NormalizeKickEvent(const std::string &event, const json &outer, NormalizedEvent &ev)
{
	const json d = Chat::PusherInnerData(outer);
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
		ev.months = static_cast<int>(NumLoose(d, "months")); // cumulative; omitted from JSON when 0
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
		ev.amount = NumLoose(d, "number_viewers");
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
		ev.amount = NumLoose(msg, "numberOfViewers");
		const std::string mid = Str(msg, "id");
		ev.id = mid.empty() ? ("kick:raid:" + ev.actorName + ":" + std::to_string(ev.ts))
				    : ("kick:raid:" + mid);
		return true;
	}

	if (event == kFollowersUpdated) {
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
		// created_at is an integer tick count (not a date) -> a stable per-follow suffix, so
		// a redelivery KickDeliveryDedupe let through still dedupes in the store.
		const int64_t createdAt = NumLoose(d, "created_at");
		ev.id = "kick:follow:" + username + ":" +
			(createdAt != 0 ? std::to_string(createdAt) : std::to_string(ev.ts));
		return true;
	}

	if (event == "KicksGifted") {
		// Kicks are Kick's paid gift currency, and `amount` counts them -- never money, since
		// Kick publishes no exchange rate. This event has no App\Events\ form.
		const json &gift = Obj(d, "gift");
		ev.amount = NumLoose(gift, "amount");
		if (ev.amount <= 0) {
			return false;
		}
		const json &sender = Obj(d, "sender");
		const std::string username = Str(sender, "username");
		ev.type = "kicks";
		ev.actorName = username.empty() ? "Anonymous" : username;
		ev.actorColor = KickSenderColor(sender);
		ev.tier = Str(gift, "name"); // the gift's display name, e.g. "Rage Quit"
		ev.message = Str(d, "message");
		// Without a transaction id the id carries receipt time, like the follow's fallback;
		// KickDeliveryDedupe has already dropped the channel_<id> / channel.<id> twin.
		const std::string txn = Chat::KickIdField(d, "gift_transaction_id");
		ev.id = !txn.empty() ? ("kick:kicks:" + txn)
				     : ("kick:kicks:" + ev.actorName + ":" + std::to_string(ev.amount) + ":" +
					std::to_string(ev.ts));
		return true;
	}

	return false; // ChatMessageEvent, bans, pins, reactions, count-only follower pings, ...
}

bool KickDeliveryDedupe::IsDuplicate(const json &frame, int64_t nowMs)
{
	const std::string channel = Str(frame, "channel");
	if (channel.rfind("channel.", 0) != 0 && channel.rfind("channel_", 0) != 0) {
		return false;
	}
	const json &data = Obj(frame, "data");
	if (data.is_null()) {
		return false;
	}
	const size_t key = std::hash<std::string>{}(Str(frame, "event") + '\n' +
						    (data.is_string() ? data.get<std::string>() : data.dump()));

	while (!recent_.empty() && nowMs - recent_.front().receivedMs > kWindowMs) {
		recent_.pop_front();
	}
	for (auto it = recent_.begin(); it != recent_.end(); ++it) {
		if (it->key == key && it->channel != channel) {
			recent_.erase(it);
			return true;
		}
	}
	if (recent_.size() >= kMaxRemembered) {
		recent_.pop_front();
	}
	recent_.push_back(Delivery{key, channel, nowMs});
	return false;
}

namespace {

// A FollowersUpdated push carries the channel's LIVE follower total (followersCount).
// Kick exposes no REST follower endpoint, so this push is the ONLY source of the number;
// feed it into the same channels.stats path the audience poller uses (Task 4) so the
// Channels panel shows a live Kick figure while streaming. This fires for the count-only
// (nameless) ping too -- which NormalizeKickEvent drops -- so the number updates on every follow
// AND unfollow, independent of the normalized follow event. Field-scoped store write
// (never round-trips access/refresh, so a concurrent token refresh isn't clobbered) plus
// the alive-guarded PostToUi so a late emit after Shutdown is dropped, never touching CEF.
// Best-effort: a missing / mistyped count does nothing (no update, no emit).
void EmitKickFollowerCount(const json &outer, const OAuth::OAuthAccount &acct)
{
	const json d = Chat::PusherInnerData(outer);
	if (!d.is_object()) {
		return;
	}
	auto it = d.find("followersCount");
	if (it == d.end() || !it->is_number()) {
		return; // count absent / mistyped -> non-fatal
	}
	const int64_t pushedCount = it->get<int64_t>();
	const std::string accountId = OAuth::AccountId(acct); // providerId:userId store key

	// Skip the redundant DPAPI-encrypt + disk write (UpdateAudience always persists) and
	// the identical emit when the total hasn't changed. Freshness for a newly-opened
	// browser is covered by the poller re-emitting the cached last-known each tick.
	auto existing = OAuth::Accounts().Get(accountId);
	if (existing && existing->audienceCount == pushedCount) {
		return;
	}
	const int64_t nowNs = (int64_t)os_gettime_ns();

	OAuth::Accounts().UpdateAudience(accountId, pushedCount, OAuth::AudienceKind::Followers, false, nowNs);

	json perAccount = json::object();
	perAccount[accountId] = json{
		{"audienceCount", pushedCount},
		{"audienceKind", "followers"},
		{"audienceHidden", false},
		{"audienceUpdatedNs", nowNs},
	};
	AsyncTask::PostToUi([p = json{{"perAccount", std::move(perAccount)}}]() mutable {
		Bridge::EmitEvent(EventNames::kChannelsStats, p);
	});
}

} // namespace

bool KickEvents::connect(const EventContext &ctx, OAuth::OAuthAccount &acct, std::string &err)
{
	// Serialize against an overlapping re-Start: the EventHub does not join old workers,
	// so a prior connect() might still be unwinding -- block until it releases the socket.
	std::lock_guard<std::mutex> run(runMutex_);
	stopped_.store(false);

	const auto canceled = [&] {
		return stopped_.load() || (ctx.canceled && ctx.canceled());
	};

	if (!Chat::WsClient::WebSocketsSupported()) {
		// Build-level: retrying can never help.
		return FailPermanent(ctx, err, "libcurl lacks WebSocket support; Kick events unavailable");
	}

	const std::string slug = acct.login; // the Kick channel slug (= account login)
	if (slug.empty()) {
		// Hard misconfiguration; retrying can never help.
		return FailPermanent(ctx, err, "Kick events: channel slug unavailable; reconnect the account");
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
			if (!Chat::LockedRecv(wsMutex_, ws_, frame, isText, recvErr)) {
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

			if (event == Chat::kPusherConnectionEstablished) {
				// Subscribe to the chatroom channel (sub/gift/host) and both channel
				// formats (followers, Kicks). channel. and channel_ are alternate spellings
				// KickLib binds both of; dedupe_ drops the second spelling's copy.
				const std::array<std::string, 3> channels = {
					Chat::PusherChatroomChannel(chatroomId),
					channelId.empty() ? std::string() : "channel." + channelId,
					channelId.empty() ? std::string() : "channel_" + channelId,
				};
				std::lock_guard<std::mutex> lock(wsMutex_);
				for (const std::string &ch : channels) {
					if (ch.empty()) {
						continue;
					}
					ws_.sendText(Chat::PusherSubscribeFrame(ch));
				}
			} else if (event == Chat::kPusherSubscriptionSucceeded) {
				backoff.reset(); // a successful subscribe proves a healthy connection
				ReportHealth(ctx, Transports::TransportHealth::State::Connected);
			} else if (event == Chat::kPusherPing) {
				std::lock_guard<std::mutex> lock(wsMutex_);
				ws_.sendText(Chat::kPusherPongFrame);
			} else if (event == Chat::kPusherError) {
				HostLog("[events] kick pusher error: " + frame);
			} else {
				if (dedupe_.IsDuplicate(outer, (int64_t)(os_gettime_ns() / 1000000))) {
					continue; // the other channel spelling already delivered this one
				}
				if (event == kFollowersUpdated) {
					// Feed the live follower total into channels.stats regardless of
					// whether this ping also names a follower (NormalizeKickEvent drops nameless
					// ones).
					EmitKickFollowerCount(outer, acct);
				}
				NormalizedEvent ev;
				if (NormalizeKickEvent(event, outer, ev)) {
					ctx.emit(ev);
				}
				// Unknown event names (chat messages, bans, ...) are ignored.
			}
		}

		Chat::LockedClose(wsMutex_, ws_);
		if (canceled()) {
			break;
		}
		HostLog(std::string("[events] kick: connection lost") + (dropErr.empty() ? "" : (": " + dropErr)));
		if (Chat::CancelableSleep(backoff.next(), canceled)) {
			break;
		}
	}

	Chat::LockedClose(wsMutex_, ws_);
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
