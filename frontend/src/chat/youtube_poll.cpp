#include "youtube_poll.hpp"

#include <cctype>
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

// A text node as InnerTube renders it: either {simpleText} or {runs:[...]}, each run a text
// or an emoji. An emoji keeps its own character, so "Awesome 🔥🔥" reads back as typed.
std::string RunsText(const json &node)
{
	const std::string simple = JsonUtil::Str(node, "simpleText");
	if (!simple.empty()) {
		return simple;
	}
	std::string out;
	const json &runs = JsonUtil::Obj(node, "runs");
	if (!runs.is_array()) {
		return out;
	}
	for (const json &run : runs) {
		const json &emoji = JsonUtil::Obj(run, "emoji");
		if (emoji.is_object()) {
			out += JsonUtil::Str(emoji, "emojiId");
		} else {
			out += JsonUtil::Str(run, "text");
		}
	}
	return out;
}

// The count in a header text like "123 votes" or "Poll · 1,234 votes": the digits (commas
// allowed) directly before the word "vote". Null when there are none.
json VoteCount(const std::string &text)
{
	const size_t word = text.find("vote");
	if (word == std::string::npos) {
		return json(nullptr);
	}
	size_t end = word;
	while (end > 0 && text[end - 1] == ' ') {
		--end;
	}
	size_t begin = end;
	while (begin > 0 && (std::isdigit(static_cast<unsigned char>(text[begin - 1])) || text[begin - 1] == ',')) {
		--begin;
	}
	std::string digits;
	for (size_t k = begin; k < end; ++k) {
		if (text[k] != ',') {
			digits += text[k];
		}
	}
	if (digits.empty() || digits.size() > 15) {
		return json(nullptr);
	}
	return json(static_cast<int64_t>(std::stoll(digits)));
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

json FromInnerTube(const json &pollRenderer)
{
	json options = json::array();
	const json &choices = JsonUtil::Obj(pollRenderer, "choices");
	if (choices.is_array()) {
		for (const json &choice : choices) {
			const json &raw = JsonUtil::Obj(choice, "voteRatio");
			json ratio = nullptr;
			if (raw.is_number()) {
				const double value = raw.get<double>();
				if (std::isfinite(value) && value >= 0 && value <= 1) {
					ratio = value;
				}
			}
			options.push_back(json{{"text", RunsText(JsonUtil::Obj(choice, "text"))}, {"ratio", ratio}});
		}
	}
	const json &header = JsonUtil::Obj(JsonUtil::Obj(pollRenderer, "header"), "pollHeaderRenderer");
	return json{{"options", std::move(options)},
		    {"totalVotes", VoteCount(RunsText(JsonUtil::Obj(header, "metadataText")))}};
}

} // namespace YouTubePoll
