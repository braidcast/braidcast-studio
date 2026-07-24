#include "youtube_events.hpp"

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "util/http_client.hpp"
#include "util/json_util.hpp"
#include "../log.hpp"
#include "../oauth/youtube_provider.hpp"
#include "../obs_bootstrap.hpp"
#include "util/time_util.hpp"

namespace Events {

using json = nlohmann::json;

namespace {

// superChatEvents.list returns recent Super Chats + Super Stickers for the channel;
// subscriptions.list?myRecentSubscribers gives the channel's newest subscribers. Both
// read even when the channel is not live, which is what gives pre-live history. Both
// are covered by youtube.force-ssl (the provider's existing scope) -- no new scope.
const char *kSuperChatEventsUrl = "https://www.googleapis.com/youtube/v3/superChatEvents";
const char *kSubscriptionsUrl = "https://www.googleapis.com/youtube/v3/subscriptions";

// How many of each to pull per pass. Small: this is a "recent history" seed, not a
// backfill of the whole channel, and the store dedupes across passes anyway.
constexpr int kMaxResults = 15;

// Whole-request timeout for each Data API GET.
constexpr int kTimeoutSec = 10;

// Live-aware poll cadences (see pollIntervalMs in the header).
constexpr int kLivePollIntervalMs = 90000;
constexpr int kIdlePollIntervalMs = 900000;

using JsonUtil::Bool;
using JsonUtil::NumLoose;
using JsonUtil::Obj;
using JsonUtil::Str;
using TimeUtil::NowMs;
using TimeUtil::Rfc3339ToEpochMs;

} // namespace

void YouTubeEvents::collect(const EventContext &ctx, OAuth::OAuthAccount &acct,
			    const std::function<void(NormalizedEvent &&)> &sink)
{
	const auto canceled = [&] {
		return stopped_.load() || (ctx.canceled && ctx.canceled());
	};

	// The shared daily-quota gate: while exhausted, skip the whole pass without
	// spending a request or logging per tick (the exhaustion itself was HostLog'd
	// once by the provider when first detected).
	if (provider_->QuotaExhausted()) {
		DBG(LogCat::Events, "youtube: quota exhausted, skipping REST poll");
		return;
	}

	// Small helper: one authed GET, tolerant of a 401 (SendAuthed refreshes) and
	// graceful on any other non-2xx (missing enablement / disabled feature / quota) --
	// events are best-effort, so a failure logs and yields nothing rather than aborting.
	auto fetch = [&](const char *what, const std::string &url, json &out) -> bool {
		if (canceled()) {
			return false;
		}
		Http::HttpReq req;
		req.method = "GET";
		req.url = url;
		req.timeoutSec = kTimeoutSec;

		Http::HttpResponse resp;
		std::string err;
		if (!provider_->SendAuthed(acct, req, resp, err)) {
			HostLog(std::string("[events] youtube: ") + what + " request failed: " + err);
			return false;
		}
		if (resp.status < 200 || resp.status >= 300) {
			// 403 = Super Chat not enabled / channel not eligible / quota (the reason
			// disambiguates; a quota-class 403 also armed the provider's shared gate in
			// SendAuthed, so the next pass skips instead of re-hammering). Non-fatal
			// either way: log and skip this source this pass.
			const std::string reason = OAuth::YouTubeErrorReason(resp.body);
			HostLog(std::string("[events] youtube: ") + what + " skipped (HTTP " +
				std::to_string(resp.status) + (reason.empty() ? "" : ", " + reason) + ")");
			return false;
		}
		out = json::parse(resp.body, nullptr, false);
		return true;
	};

	// 1) Super Chats (and Super Stickers, which superChatEvents also reports). Each item
	//    carries its own event id + a snippet with the amount/currency/comment/supporter.
	//    Only when NOT live: during a broadcast the chat forward supplies these in real
	//    time under the liveChatMessage id, and the REST superChatEvent id differs, so
	//    running both would emit each Super Chat twice. Splitting by live-state keeps the
	//    paths disjoint. Residual edge (acceptable): connecting mid-broadcast won't
	//    REST-backfill that stream's Super Chats from before connect -- the live forward
	//    only catches ones from connect-time onward.
	if (!provider_->LiveChatActive()) {
		json j;
		const std::string url =
			std::string(kSuperChatEventsUrl) + "?part=snippet&maxResults=" + std::to_string(kMaxResults);
		if (fetch("superChatEvents", url, j)) {
			const json &items = Obj(j, "items");
			if (items.is_array()) {
				for (const json &item : items) {
					if (canceled()) {
						return;
					}
					const std::string itemId = Str(item, "id");
					if (itemId.empty()) {
						continue; // no id -> undedupable, drop
					}
					const json &snippet = Obj(item, "snippet");
					// superChatEvents.list reports Super Stickers too; label them like the live
					// path so the two content keys align (else a sticker would still double).
					const std::string type = Bool(snippet, "isSuperStickerEvent") ? "supersticker"
												      : "superchat";
					const int64_t micros = NumLoose(snippet, "amountMicros");
					const int64_t createdMs = Rfc3339ToEpochMs(Str(snippet, "createdAt"));
					const std::string supporterChannelId =
						Str(Obj(snippet, "supporterDetails"), "channelId");

					NormalizedEvent ev;
					ev.platform = "youtube";
					ev.type = type;
					// Content-derived id, identical to the live-chat forward's key, so a Super
					// Chat seen live and then re-fetched by this REST poll collapses in the store
					// (the two API surfaces assign different resource ids). Fall back to the
					// resource id only when the supporter channel is absent (rare) -- that item
					// then won't cross-path-dedupe, an accepted edge.
					ev.id = supporterChannelId.empty()
							? ("youtube:" + type + ":" + itemId)
							: YouTubeMoneyEventId(type, supporterChannelId, micros,
									      createdMs / 1000);
					ev.actorName = Str(Obj(snippet, "supporterDetails"), "displayName");
					// amountMicros is micros of the currency; store MINOR units (cents) for the
					// dock to format: micros / 10000 = cents.
					ev.amount = micros / 10000;
					ev.currency = Str(snippet, "currency");
					ev.message = Str(snippet, "commentText");
					ev.ts = createdMs;
					sink(std::move(ev));
				}
			}
		}
	}

	// 2) Recent subscribers. A YouTube "subscribe" is a free follow, not a paid
	//    membership -> type "follow" (memberships arrive as newSponsor via the live chat).
	{
		json j;
		const std::string url =
			std::string(kSubscriptionsUrl) +
			"?part=subscriberSnippet&myRecentSubscribers=true&maxResults=" + std::to_string(kMaxResults);
		if (fetch("subscriptions", url, j)) {
			const json &items = Obj(j, "items");
			if (items.is_array()) {
				for (const json &item : items) {
					if (canceled()) {
						return;
					}
					const std::string itemId = Str(item, "id");
					if (itemId.empty()) {
						continue;
					}
					const json &subSnippet = Obj(item, "subscriberSnippet");
					NormalizedEvent ev;
					ev.platform = "youtube";
					ev.type = "follow";
					ev.id = "youtube:sub:" + itemId;
					ev.actorName = Str(subSnippet, "title");
					// subscriptions carries no timestamp we can trust for ordering -> receipt time.
					ev.ts = NowMs();
					sink(std::move(ev));
				}
			}
		}
	}
}

bool YouTubeEvents::connect(const EventContext &ctx, OAuth::OAuthAccount &acct, std::string &err)
{
	// No real-time socket: YouTube's push events arrive via the live chat sink
	// (youtube_chat forwards them to Events::Hub().Ingest). The hub loop drives poll()
	// on the pollIntervalMs() cadence after this returns, so return cleanly at once.
	(void)acct;
	stopped_.store(false);
	err.clear();
	// No socket to establish, but the REST transport is armed and polling: report it
	// Connected so the health surface reflects an active account rather than a
	// perpetual "connecting".
	if (ctx.reportHealth) {
		ctx.reportHealth(Transports::TransportHealth::State::Connected, "");
	}
	return true;
}

bool YouTubeEvents::backfill(const EventContext &ctx, OAuth::OAuthAccount &acct, std::vector<NormalizedEvent> &out,
			     std::string &err)
{
	(void)err; // best-effort: collect() logs + skips per-source failures, never aborts
	stopped_.store(false);
	collect(ctx, acct, [&out](NormalizedEvent &&ev) { out.push_back(std::move(ev)); });
	return true;
}

void YouTubeEvents::poll(const EventContext &ctx, OAuth::OAuthAccount &acct)
{
	// Emit straight into the store (dedupe makes re-fetching the same items harmless).
	collect(ctx, acct, [&ctx](NormalizedEvent &&ev) { ctx.emit(ev); });
}

int YouTubeEvents::pollIntervalMs()
{
	return ObsBootstrap::AnyOutputLive() ? kLivePollIntervalMs : kIdlePollIntervalMs;
}

} // namespace Events
