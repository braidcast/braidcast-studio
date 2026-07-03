#include "twitch_events.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <utility>

#include <nlohmann/json.hpp>

#include "../http_client.hpp"
#include "../log.hpp"
#include "../oauth/twitch_provider.hpp"

namespace Events {

using json = nlohmann::json;

namespace {

const char *kEventSubUrl = "wss://eventsub.wss.twitch.tv/ws";
const char *kSubscribeUrl = "https://api.twitch.tv/helix/eventsub/subscriptions";
const char *kFollowersUrl = "https://api.twitch.tv/helix/channels/followers";

// How long to wait for the session_welcome after the socket opens before treating
// the connection as dead and reconnecting -- bounds a socket that upgrades but never
// speaks the EventSub handshake.
constexpr int64_t kWelcomeTimeoutMs = 15000;

// Extra grace on top of the session's advertised keepalive_timeout_seconds before
// the watchdog declares the connection stale: EventSub sends a keepalive every
// keepalive window, so no frame for keepalive + margin means the link is gone.
constexpr int64_t kKeepaliveMarginMs = 10000;

int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

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

bool Bool(const json &j, const char *key)
{
	if (!j.is_object()) {
		return false;
	}
	auto it = j.find(key);
	return it != j.end() && it->is_boolean() && it->get<bool>();
}

int64_t Num(const json &j, const char *key)
{
	if (!j.is_object()) {
		return 0;
	}
	auto it = j.find(key);
	return (it != j.end() && it->is_number()) ? it->get<int64_t>() : 0;
}

// Return a reference to `j[key]` when `j` is an object holding it, else a shared null
// json -- lets the nested-field accessors chain (Obj(Obj(msg,"payload"),"session"))
// without intermediate copies or null checks at each hop.
const json &Obj(const json &j, const char *key)
{
	static const json kNull = json(nullptr);
	if (!j.is_object()) {
		return kNull;
	}
	auto it = j.find(key);
	return it == j.end() ? kNull : *it;
}

// Parse an RFC3339 / ISO-8601 UTC instant ("2024-01-02T03:04:05.678Z") into epoch
// milliseconds; falls back to the current wall clock on a parse failure so an event
// never carries a zero timestamp. Mirrors youtube_chat's parser (MSVC _mkgmtime).
int64_t Rfc3339ToEpochMs(const std::string &iso)
{
	int y = 0, mon = 0, d = 0, h = 0, mi = 0, s = 0;
	if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mon, &d, &h, &mi, &s) == 6) {
		std::tm tm{};
		tm.tm_year = y - 1900;
		tm.tm_mon = mon - 1;
		tm.tm_mday = d;
		tm.tm_hour = h;
		tm.tm_min = mi;
		tm.tm_sec = s;
		const std::time_t epoch = _mkgmtime(&tm);
		if (epoch != static_cast<std::time_t>(-1)) {
			int64_t millis = 0;
			const auto dot = iso.find('.');
			if (dot != std::string::npos) {
				std::string frac;
				for (size_t i = dot + 1;
				     i < iso.size() && std::isdigit(static_cast<unsigned char>(iso[i])) && frac.size() < 3;
				     ++i) {
					frac.push_back(iso[i]);
				}
				while (frac.size() < 3) {
					frac.push_back('0');
				}
				millis = std::stoll(frac);
			}
			return static_cast<int64_t>(epoch) * 1000 + millis;
		}
	}
	return NowMs();
}

// Map Twitch's numeric sub tier ("1000"/"2000"/"3000") to a human label; pass any
// other value (e.g. "Prime") through unchanged.
std::string TierLabel(const std::string &tier)
{
	if (tier == "1000") {
		return "Tier 1";
	}
	if (tier == "2000") {
		return "Tier 2";
	}
	if (tier == "3000") {
		return "Tier 3";
	}
	return tier;
}

// The condition shape a subscription's `condition` object takes: the plain
// broadcaster-scoped form, the follow-v2 form (adds a moderator id), and the raid
// form (keyed on the raid target broadcaster).
enum class Cond { Broadcaster, Follow, Raid };

struct SubSpec {
	const char *type;
	const char *version;
	Cond cond;
};

// The subscriptions to create on session_welcome. channel.subscribe fires for gifted
// subs too (is_gift=true), which the channel.subscription.gift event already covers,
// so the normalizer drops the gift half of channel.subscribe to avoid a double.
const std::array<SubSpec, 6> kSubs = {{
	{"channel.follow", "2", Cond::Follow},
	{"channel.subscribe", "1", Cond::Broadcaster},
	{"channel.subscription.message", "1", Cond::Broadcaster},
	{"channel.subscription.gift", "1", Cond::Broadcaster},
	{"channel.cheer", "1", Cond::Broadcaster},
	{"channel.raid", "1", Cond::Raid},
}};

json BuildCondition(Cond cond, const std::string &broadcasterId)
{
	switch (cond) {
	case Cond::Follow:
		// v2 requires the moderator id; the broadcaster moderates their own channel.
		return json{{"broadcaster_user_id", broadcasterId}, {"moderator_user_id", broadcasterId}};
	case Cond::Raid:
		return json{{"to_broadcaster_user_id", broadcasterId}};
	case Cond::Broadcaster:
	default:
		return json{{"broadcaster_user_id", broadcasterId}};
	}
}

// Normalize one EventSub `notification` envelope into `ev`. Returns false when the
// event carries no message id (undedupable), is an unhandled type, or is the gift
// half of a channel.subscribe (covered by channel.subscription.gift). The dedupe id
// is the delivery-unique metadata.message_id, which Twitch guarantees stable per
// notification, so a redelivery collapses in the store.
bool Normalize(const json &msg, NormalizedEvent &ev)
{
	const json &meta = Obj(msg, "metadata");
	const std::string messageId = Str(meta, "message_id");
	if (messageId.empty()) {
		return false;
	}
	const std::string subType = Str(meta, "subscription_type");
	const json &event = Obj(Obj(msg, "payload"), "event");
	const int64_t baseTs = Rfc3339ToEpochMs(Str(meta, "message_timestamp"));

	ev.platform = "twitch";
	ev.id = "twitch:" + messageId;
	ev.ts = baseTs;

	if (subType == "channel.follow") {
		ev.type = "follow";
		ev.actorName = Str(event, "user_name");
		const std::string followedAt = Str(event, "followed_at");
		if (!followedAt.empty()) {
			ev.ts = Rfc3339ToEpochMs(followedAt);
		}
	} else if (subType == "channel.subscribe") {
		if (Bool(event, "is_gift")) {
			return false; // the gift is emitted via channel.subscription.gift
		}
		ev.type = "sub";
		ev.actorName = Str(event, "user_name");
		ev.tier = TierLabel(Str(event, "tier"));
	} else if (subType == "channel.subscription.message") {
		ev.type = "resub";
		ev.actorName = Str(event, "user_name");
		ev.tier = TierLabel(Str(event, "tier"));
		ev.months = static_cast<int>(Num(event, "cumulative_months"));
		ev.message = Str(Obj(event, "message"), "text");
	} else if (subType == "channel.subscription.gift") {
		ev.type = "subgift";
		ev.actorName = Bool(event, "is_anonymous") ? "Anonymous" : Str(event, "user_name");
		ev.count = static_cast<int>(Num(event, "total"));
		ev.tier = TierLabel(Str(event, "tier"));
	} else if (subType == "channel.cheer") {
		ev.type = "cheer";
		ev.actorName = Bool(event, "is_anonymous") ? "Anonymous" : Str(event, "user_name");
		ev.amount = Num(event, "bits");
		ev.message = Str(event, "message");
	} else if (subType == "channel.raid") {
		ev.type = "raid";
		ev.actorName = Str(event, "from_broadcaster_user_name");
		ev.amount = Num(event, "viewers");
	} else {
		return false; // unhandled subscription type
	}
	return true;
}

} // namespace

void TwitchEvents::createSubscriptions(OAuth::OAuthAccount &acct, const std::string &broadcasterId,
				       const std::string &sessionId, const std::function<bool()> &canceled)
{
	for (const SubSpec &spec : kSubs) {
		if (canceled && canceled()) {
			return;
		}
		const json body = json{
			{"type", spec.type},
			{"version", spec.version},
			{"condition", BuildCondition(spec.cond, broadcasterId)},
			{"transport", json{{"method", "websocket"}, {"session_id", sessionId}}},
		};

		Http::HttpReq req;
		req.method = "POST";
		req.url = kSubscribeUrl;
		req.contentType = "application/json";
		req.body = body.dump();
		req.timeoutSec = 10;

		Http::HttpResponse resp;
		std::string serr;
		if (!provider_->SendAuthed(acct, req, resp, serr)) {
			HostLog("[events] twitch: subscribe " + std::string(spec.type) + " failed: " + serr);
			continue;
		}
		if (resp.status >= 200 && resp.status < 300) {
			HostLog("[events] twitch: subscribed " + std::string(spec.type));
		} else {
			// 401/403 = dead token / missing scope -> graceful skip; log others too.
			HostLog("[events] twitch: subscribe " + std::string(spec.type) + " skipped (HTTP " +
				std::to_string(resp.status) + ")");
		}
	}
}

bool TwitchEvents::backfill(const EventContext &ctx, OAuth::OAuthAccount &acct, std::vector<NormalizedEvent> &out,
			    std::string &err)
{
	const auto canceled = [&] { return stopped_.load() || (ctx.canceled && ctx.canceled()); };

	const std::string broadcasterId = acct.userId;
	if (broadcasterId.empty() || canceled()) {
		return true; // nothing to seed (identity resolves at connect; empty is non-fatal)
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kFollowersUrl) + "?broadcaster_id=" + Http::UrlEncode(broadcasterId) + "&first=20";
	req.timeoutSec = 10;

	Http::HttpResponse resp;
	if (!provider_->SendAuthed(acct, req, resp, err)) {
		err.clear();
		return true; // best-effort backfill; a transport/re-auth failure is non-fatal
	}
	if (resp.status == 401 || resp.status == 403) {
		return true; // no moderator:read:followers scope yet -> no history, non-fatal
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "Twitch followers backfill failed (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	const json j = json::parse(resp.body, nullptr, false);
	const json &data = Obj(j, "data");
	if (!data.is_array()) {
		return true;
	}
	for (const json &row : data) {
		NormalizedEvent ev;
		ev.platform = "twitch";
		ev.type = "follow";
		ev.actorName = Str(row, "user_name");
		const std::string userId = Str(row, "user_id");
		// Backfill has no message id; key on the follower user so a re-seed on the next
		// connect dedupes. A live channel.follow uses the message-id scheme, so the same
		// follower never double-counts (you cannot already-follow and newly-follow).
		ev.id = "twitch:follow:" + (userId.empty() ? ev.actorName : userId);
		ev.ts = Rfc3339ToEpochMs(Str(row, "followed_at"));
		out.push_back(std::move(ev));
	}
	return true;
}

bool TwitchEvents::connect(const EventContext &ctx, OAuth::OAuthAccount &acct, std::string &err)
{
	// Serialize against an overlapping re-Start: the hub does not join old workers, so a
	// prior connect() might still be unwinding -- block until it releases the socket.
	std::lock_guard<std::mutex> run(runMutex_);
	stopped_.store(false);

	const auto canceled = [&] { return stopped_.load() || (ctx.canceled && ctx.canceled()); };

	const std::string broadcasterId = acct.userId;
	if (broadcasterId.empty()) {
		// The account-connect lifecycle only starts accounts that ran fetchIdentity, so
		// this is a hard misconfiguration rather than a transient -- mark it permanent so
		// the hub stops retrying instead of subscribing with an empty condition.
		err = "Twitch events: broadcaster user id unavailable; reconnect the account";
		if (ctx.markFatal) {
			ctx.markFatal();
		}
		return false;
	}

	Chat::Backoff backoff;
	std::string reconnectUrl; // set by session_reconnect; consumed once by the next open

	while (!canceled()) {
		const bool isReconnect = !reconnectUrl.empty();
		const std::string url = isReconnect ? reconnectUrl : std::string(kEventSubUrl);
		reconnectUrl.clear();

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

		// Read frames until the session_welcome hands us the session id + keepalive
		// window. Bounded by kWelcomeTimeoutMs so a silent socket can't stall here.
		std::string sessionId;
		int64_t keepaliveMs = 10000;
		bool welcomed = false;
		const int64_t welcomeDeadline = NowMs() + kWelcomeTimeoutMs;
		while (!canceled() && !welcomed) {
			// Enforce the welcome deadline at the top so a stream of non-welcome text
			// frames before the handshake cannot bypass it (protocol guarantees
			// welcome-first, so this is a defensive bound).
			if (NowMs() > welcomeDeadline) {
				err = "Twitch events: no session_welcome";
				break;
			}
			std::string frame;
			bool isText = false;
			std::string rerr;
			bool ok;
			{
				std::lock_guard<std::mutex> lock(wsMutex_);
				ok = ws_.recv(frame, isText, rerr);
			}
			if (!ok) {
				err = rerr;
				break;
			}
			if (frame.empty() || !isText) {
				continue;
			}
			const json msg = json::parse(frame, nullptr, false);
			if (Str(Obj(msg, "metadata"), "message_type") == "session_welcome") {
				const json &session = Obj(Obj(msg, "payload"), "session");
				sessionId = Str(session, "id");
				const int64_t k = Num(session, "keepalive_timeout_seconds");
				if (k > 0) {
					keepaliveMs = k * 1000;
				}
				welcomed = !sessionId.empty();
				if (!welcomed) {
					err = "Twitch events: session_welcome missing session id";
					break;
				}
			}
		}

		if (!welcomed) {
			{
				std::lock_guard<std::mutex> lock(wsMutex_);
				ws_.close();
			}
			if (canceled()) {
				break;
			}
			if (Chat::CancelableSleep(backoff.next(), canceled)) {
				break;
			}
			continue;
		}
		backoff.reset(); // a welcome proves a healthy connection

		// A session_reconnect socket resumes the SAME subscriptions, so only a fresh
		// connection creates them.
		if (!isReconnect) {
			createSubscriptions(acct, broadcasterId, sessionId, canceled);
		}

		// Read loop. recv() polls with a ~250ms timeout so canceled() and the keepalive
		// watchdog are re-checked frequently.
		int64_t lastMsg = NowMs();
		const int64_t staleAfterMs = keepaliveMs + kKeepaliveMarginMs;
		bool reconnecting = false;
		while (!canceled()) {
			std::string frame;
			bool isText = false;
			std::string rerr;
			bool ok;
			{
				std::lock_guard<std::mutex> lock(wsMutex_);
				ok = ws_.recv(frame, isText, rerr);
			}
			if (!ok) {
				err = rerr;
				break; // peer closed / transport error -> reconnect
			}
			if (frame.empty() || !isText) {
				if (NowMs() - lastMsg > staleAfterMs) {
					err = "Twitch events: keepalive timeout";
					break;
				}
				continue;
			}
			lastMsg = NowMs();

			const json msg = json::parse(frame, nullptr, false);
			const std::string type = Str(Obj(msg, "metadata"), "message_type");
			if (type == "session_keepalive") {
				continue; // liveness only
			}
			if (type == "notification") {
				NormalizedEvent ev;
				if (Normalize(msg, ev)) {
					ctx.emit(ev);
				}
				continue;
			}
			if (type == "session_reconnect") {
				reconnectUrl = Str(Obj(Obj(msg, "payload"), "session"), "reconnect_url");
				reconnecting = !reconnectUrl.empty();
				break;
			}
			if (type == "revocation") {
				// Twitch dropped this subscription (user_removed / version_removed /
				// authorization_revoked) but keeps the socket + keepalives alive, so the
				// watchdog never fires and the type would silently go dead. Break to a
				// FRESH reconnect (reconnectUrl empty -> isReconnect false) so the next
				// session_welcome re-runs createSubscriptions, recovering it. A truly dead
				// token just 403-skips every re-POST and settles connected-but-idle (no
				// tight loop, as no further revocations arrive with nothing subscribed).
				const json &sub = Obj(Obj(msg, "payload"), "subscription");
				HostLog("[events] twitch: subscription revoked (" + Str(sub, "type") + ", status=" +
					Str(sub, "status") + ") -> reconnecting to re-subscribe");
				break;
			}
		}

		{
			std::lock_guard<std::mutex> lock(wsMutex_);
			ws_.close();
		}
		if (canceled()) {
			break;
		}
		if (reconnecting) {
			continue; // reconnect immediately to the new URL, subscriptions carry over
		}
		if (Chat::CancelableSleep(backoff.next(), canceled)) {
			break;
		}
	}

	{
		std::lock_guard<std::mutex> lock(wsMutex_);
		ws_.close();
	}
	if (canceled()) {
		err.clear(); // clean cancel: hub suppresses the log
	}
	return false;
}

void TwitchEvents::disconnect()
{
	// Only flip the flag: the worker that owns ws_ tears the socket down on its own
	// thread. recv()'s ~250ms poll makes the loop notice promptly.
	stopped_.store(true);
}

} // namespace Events
