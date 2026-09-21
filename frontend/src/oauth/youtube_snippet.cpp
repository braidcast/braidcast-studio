#include "youtube_snippet.hpp"

#include <algorithm>
#include <cctype>

namespace YouTubeSnippet {
namespace {

using json = nlohmann::json;

// The snippet properties a videos.update may write. Everything else a videos.list returns
// (publishedAt, channelId, thumbnails, liveBroadcastContent, localized, ...) is read-only.
constexpr const char *kWritable[] = {"title", "description",     "categoryId",
				     "tags",  "defaultLanguage", "defaultAudioLanguage"};

constexpr const char *kLanguageKeys[] = {"defaultLanguage", "defaultAudioLanguage"};

// ISO 639-2 "no linguistic content". YouTube hands it back on read and refuses it on write,
// so a read-modify-write that carries it over fails the whole update.
constexpr const char *kNotApplicable = "zxx";

// Required on every snippet update. Used only when neither the edit nor the video has one,
// which a video YouTube created itself does not normally reach.
constexpr const char *kFallbackCategoryId = "24"; // Entertainment

bool HasString(const json &obj, const char *key)
{
	return obj.is_object() && obj.contains(key) && obj[key].is_string() && !obj[key].get<std::string>().empty();
}

std::string Lower(std::string s)
{
	for (char &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

} // namespace

json EditDigestSource(const Edit &edit)
{
	return json{
		{"title", edit.title},           {"description", edit.description},
		{"categoryId", edit.categoryId}, {"tags", edit.tagsStated ? edit.tags : json()},
		{"language", edit.language},     {"languageExplicit", edit.languageExplicit},
	};
}

json Merge(const json &current, const Edit &edit)
{
	json out = json::object();
	if (current.is_object()) {
		for (const char *key : kWritable) {
			if (current.contains(key)) {
				out[key] = current[key];
			}
		}
	}
	for (const char *key : kLanguageKeys) {
		if (out.contains(key) && (!out[key].is_string() || out[key].get<std::string>() == kNotApplicable)) {
			out.erase(key);
		}
	}

	out["title"] = edit.title;
	out["description"] = edit.description;

	if (!edit.categoryId.empty()) {
		out["categoryId"] = edit.categoryId;
	} else if (!HasString(out, "categoryId")) {
		out["categoryId"] = kFallbackCategoryId;
	}

	if (edit.tagsStated) {
		out["tags"] = edit.tags;
	}

	if (!edit.language.empty()) {
		for (const char *key : kLanguageKeys) {
			if (edit.languageExplicit || !HasString(out, key)) {
				out[key] = edit.language;
			}
		}
	}
	return out;
}

std::string LanguageFromLocale(const std::string &localeName)
{
	std::string name = Lower(localeName);
	for (char &c : name) {
		if (c == '_') {
			c = '-';
		}
	}
	const std::string primary = name.substr(0, name.find('-'));
	// ISO 639 codes are two or three letters; "q" starts the private-use range, which is
	// where Windows keeps its pseudo-locales ("qps-ploc").
	const bool letters = !primary.empty() &&
			     std::all_of(primary.begin(), primary.end(), [](char c) { return c >= 'a' && c <= 'z'; });
	if (!letters || primary.size() < 2 || primary.size() > 3 || primary[0] == 'q') {
		return std::string();
	}
	if (primary != "zh") {
		return primary;
	}
	// An explicit script decides ("zh-Hans-HK" is Simplified in Hong Kong). Only the older
	// names that carry a region alone ("zh-TW") fall through to the region.
	const std::string tail = name.substr(primary.size()) + "-";
	if (tail.find("-hans-") != std::string::npos) {
		return "zh-Hans";
	}
	if (tail.find("-hant-") != std::string::npos) {
		return "zh-Hant";
	}
	for (const char *traditional : {"-tw-", "-hk-", "-mo-"}) {
		if (tail.find(traditional) != std::string::npos) {
			return "zh-Hant";
		}
	}
	return "zh-Hans";
}

} // namespace YouTubeSnippet
