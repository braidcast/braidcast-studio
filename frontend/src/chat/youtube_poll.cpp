#include "youtube_poll.hpp"

#include <cmath>
#include <cstdint>
#include <exception>

#include "util/json_util.hpp"

namespace YouTubePoll {

using json = nlohmann::json;

namespace {

// A vote count as either a JSON number or a numeric string (the Data API serializes some
// 64-bit counters as strings). Anything else -- absent, negative, fractional garbage, a
// string with trailing text -- is null: a tally we cannot read must not render as zero votes.
json ReadTally(const json &option)
{
	const json &raw = JsonUtil::Obj(option, "tally");
	if (raw.is_number_integer()) {
		const int64_t value = raw.get<int64_t>();
		return value >= 0 ? json(value) : json(nullptr);
	}
	if (raw.is_number_float()) {
		// Bounded before the cast: an out-of-range or non-finite double is undefined
		// behaviour to convert, and no real tally is anywhere near the limit.
		const double value = raw.get<double>();
		constexpr double kMaxTally = 9.0e18;
		return std::isfinite(value) && value >= 0 && value < kMaxTally ? json(static_cast<int64_t>(value))
									       : json(nullptr);
	}
	if (!raw.is_string()) {
		return json(nullptr);
	}
	const std::string text = raw.get<std::string>();
	if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
		return json(nullptr);
	}
	try {
		return json(static_cast<int64_t>(std::stoll(text)));
	} catch (const std::exception &) {
		return json(nullptr);
	}
}

// pollDetails sits under the snippet in the documented resource; a bare top-level copy is
// accepted too so a shape change on that one hop does not lose the result.
const json &PollDetails(const json &message)
{
	const json &inSnippet = JsonUtil::Obj(JsonUtil::Obj(message, "snippet"), "pollDetails");
	return inSnippet.is_object() ? inSnippet : JsonUtil::Obj(message, "pollDetails");
}

} // namespace

json BuildInsertBody(const std::string &liveChatId, const std::string &question,
		     const std::vector<std::string> &options)
{
	json optionRows = json::array();
	for (const std::string &option : options) {
		optionRows.push_back(json{{"optionText", option}});
	}
	return json{
		{"snippet", json{{"liveChatId", liveChatId},
				 {"type", "pollEvent"},
				 {"pollDetails", json{{"metadata", json{{"questionText", question},
									{"options", std::move(optionRows)}}}}}}},
	};
}

json Normalize(const json &message, const char *fallbackStatus)
{
	const json &details = PollDetails(message);
	const json &metadata = JsonUtil::Obj(details, "metadata");

	json options = json::array();
	const json &rawOptions = JsonUtil::Obj(metadata, "options");
	if (rawOptions.is_array()) {
		for (const json &option : rawOptions) {
			std::string text = JsonUtil::Str(option, "optionText");
			if (text.empty()) {
				text = JsonUtil::Str(option, "text");
			}
			options.push_back(json{{"text", text}, {"tally", ReadTally(option)}});
		}
	}

	std::string status = JsonUtil::Str(metadata, "status");
	if (status.empty()) {
		status = JsonUtil::Str(details, "status");
	}
	if (status != "active" && status != "closed") {
		status = fallbackStatus;
	}

	return json{{"id", JsonUtil::Str(message, "id")},
		    {"question", JsonUtil::Str(metadata, "questionText")},
		    {"options", std::move(options)},
		    {"status", status}};
}

} // namespace YouTubePoll
