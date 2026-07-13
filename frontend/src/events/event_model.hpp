#ifndef OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_MODEL_HPP_
#define OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_MODEL_HPP_

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

// The normalized cross-platform live-event model (Phase 9.2a). One struct plus an
// open `type` vocabulary so the store, hub, bridge, and dock never branch per
// platform: adding an event kind is a new `type` string (and a dock icon), never a
// new code path. Follow/sub/gift/cheer/raid/superchat/membership from every
// platform's transport funnel into this shape before they reach the store or JS.
namespace Events {

using json = nlohmann::json;

struct NormalizedEvent {
	std::string id;       // stable, platform-unique; the dedupe key
	std::string platform; // "twitch" | "youtube" | "kick"
	std::string type;     // follow|sub|resub|subgift|cheer|raid|superchat|supersticker|member
	int64_t ts = 0;       // ms since epoch (event time if the API gives one, else receipt)
	std::string actorName;
	std::string actorColor; // "" if unknown
	// Optional, per-type (omitted from JSON when empty-string / zero):
	int64_t amount = 0;   // cheer bits / superchat minor-currency units / raid viewers
	std::string currency; // superchat currency code
	std::string tier;     // sub/member tier label
	int months = 0;       // resub cumulative months
	int count = 0;        // gift count
	std::string message;  // resub/superchat user message (plain text; the dock escapes it)

	// The JS/persistence shape: always id/platform/type/ts/actorName; every other
	// field is omitted when empty-string or zero so a follow event carries no stray
	// amount/currency/... keys and the dock's optional-field summary stays simple.
	json ToJson() const
	{
		json j = json{
			{"id", id}, {"platform", platform}, {"type", type}, {"ts", ts}, {"actorName", actorName},
		};
		if (!actorColor.empty()) {
			j["actorColor"] = actorColor;
		}
		if (amount != 0) {
			j["amount"] = amount;
		}
		if (!currency.empty()) {
			j["currency"] = currency;
		}
		if (!tier.empty()) {
			j["tier"] = tier;
		}
		if (months != 0) {
			j["months"] = months;
		}
		if (count != 0) {
			j["count"] = count;
		}
		if (!message.empty()) {
			j["message"] = message;
		}
		return j;
	}
};

// Content-derived dedupe id for YouTube Super Chat / Super Sticker events. YouTube
// hands the SAME purchase two different resource ids depending on the API surface: the
// liveChatMessage id (the live-chat forward) versus the superChatEvent id (the REST
// superChatEvents.list). Keying on the resource id would double-count a purchase seen by
// both paths (concurrently, or the REST poll re-fetching it after the stream ends). So
// both paths derive their id from the purchase CONTENT instead -- supporter channel +
// amount (micros) + creation second -- which is identical across the two surfaces and
// collapses the pair in the store. `type` is "superchat" or "supersticker";
// `createdAtEpochSeconds` is floored to the second so two genuine purchases from the same
// supporter still differ while the live/REST pair for one purchase coincides.
inline std::string YouTubeMoneyEventId(const std::string &type, const std::string &supporterChannelId,
				       int64_t amountMicros, int64_t createdAtEpochSeconds)
{
	return "youtube:" + type + ":" + supporterChannelId + ":" + std::to_string(amountMicros) + ":" +
	       std::to_string(createdAtEpochSeconds);
}

} // namespace Events

#endif // OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_MODEL_HPP_
