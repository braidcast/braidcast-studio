#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_FEED_QUERY_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_FEED_QUERY_HPP_

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "../util/string_util.hpp"

// What the two paged feeds (events.list, chat.list) share: the destination filter a dock
// sends with each page request, the page-size bounds, and the reply shape. The filter is
// the host half of the webview's matchesSelection (frontend/web/src/lib/ui/
// destinationSelection.ts) and must answer exactly as it does: the dock filters the live
// frames it appends with that function and the pages it prepends with this one, so any
// disagreement shows up as rows that vanish or appear when the reader scrolls up.
namespace Feed {

using json = nlohmann::json;

// Largest page a caller may ask for; a bigger `limit` is clamped to it.
constexpr size_t kMaxLimit = 200;
// The page a caller gets when it names no `limit`.
constexpr size_t kChatPageDefault = 50;
constexpr size_t kEventPageDefault = 30;

// A platform name as filters and stored rows compare it: trimmed and lowercased, as the
// webview's platformKey() spells it.
inline std::string NormalizePlatform(const std::string &platform)
{
	return StringUtil::ToLower(StringUtil::Trim(platform));
}

struct Filter {
	enum class Kind { All, Platform, Destination };
	Kind kind = Kind::All;
	std::string platform;    // Platform: NormalizePlatform'd
	std::string profileUuid; // Destination: the selected stream profile, never empty
	std::string accountId;   // Destination: that profile's account ("" when the dock has none)

	// A row with a profileUuid belongs to that one broadcast; a row without one is
	// channel-wide and belongs to every stream of its account.
	bool Matches(const std::string &itemPlatform, const std::string &itemProfileUuid,
		     const std::string &itemAccountId) const
	{
		switch (kind) {
		case Kind::All:
			return true;
		case Kind::Platform:
			return NormalizePlatform(itemPlatform) == platform;
		case Kind::Destination:
			return itemProfileUuid == profileUuid ||
			       (itemProfileUuid.empty() && itemAccountId == accountId);
		}
		return false;
	}
};

// params.filter -> `out`. Absent or null means every row. False + `error` for a filter the
// webview could not have built, rather than guessing a wider or narrower one.
inline bool ParseFilter(const json &params, Filter &out, std::string &error)
{
	out = Filter{};
	if (!params.is_object()) {
		return true;
	}
	const auto it = params.find("filter");
	if (it == params.end() || it->is_null()) {
		return true;
	}
	if (!it->is_object()) {
		error = "filter must be an object";
		return false;
	}
	const auto str = [&](const char *key) {
		const auto v = it->find(key);
		return v != it->end() && v->is_string() ? v->get<std::string>() : std::string();
	};
	const std::string kind = str("kind");
	if (kind == "all") {
		return true;
	}
	if (kind == "platform") {
		out.kind = Filter::Kind::Platform;
		out.platform = NormalizePlatform(str("platform"));
		if (out.platform.empty()) {
			error = "a platform filter needs a platform";
			return false;
		}
		return true;
	}
	if (kind == "destination") {
		out.kind = Filter::Kind::Destination;
		out.profileUuid = str("profileUuid");
		out.accountId = str("accountId");
		if (out.profileUuid.empty()) {
			error = "a destination filter needs a profileUuid";
			return false;
		}
		return true;
	}
	error = "filter.kind must be all, platform or destination";
	return false;
}

// params.limit -> `out`, clamped to [1, kMaxLimit]; `fallback` when absent.
inline bool ParseLimit(const json &params, size_t fallback, size_t &out, std::string &error)
{
	out = fallback;
	if (!params.is_object()) {
		return true;
	}
	const auto it = params.find("limit");
	if (it == params.end() || it->is_null()) {
		return true;
	}
	if (!it->is_number_integer()) {
		error = "limit must be an integer";
		return false;
	}
	// Read unsigned values as unsigned: one past INT64_MAX must clamp to the maximum, not
	// wrap negative and clamp to 1.
	if (it->is_number_unsigned()) {
		const uint64_t n = it->get<uint64_t>();
		out = n < 1 ? 1 : n > kMaxLimit ? kMaxLimit : static_cast<size_t>(n);
		return true;
	}
	const int64_t n = it->get<int64_t>();
	out = n < 1 ? 1 : n > static_cast<int64_t>(kMaxLimit) ? kMaxLimit : static_cast<size_t>(n);
	return true;
}

// The FeedPage wire shape: items oldest-first, whether older rows exist past them, and
// the store's clear epoch the page was read under.
inline json PageJson(json items, bool more, uint64_t epoch)
{
	return json{{"items", std::move(items)}, {"more", more}, {"epoch", epoch}};
}

} // namespace Feed

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_FEED_QUERY_HPP_
