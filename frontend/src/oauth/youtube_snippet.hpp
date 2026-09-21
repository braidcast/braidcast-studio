#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_YOUTUBE_SNIPPET_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_YOUTUBE_SNIPPET_HPP_

#include <nlohmann/json.hpp>

#include <string>

// The body of a YouTube videos.update(part=snippet), built without I/O so the rules can be
// tested offline. The rules exist because part=snippet is destructive: per the API
// reference, a property that already has a value and is omitted from the request is
// DELETED. So the body starts from what the video already holds and lays the edit over
// it; sending only the fields we manage would wipe the rest, the video language among them,
// and leave YouTube to guess that again.
namespace YouTubeSnippet {

struct Edit {
	std::string title;
	std::string description;
	std::string categoryId;  // empty = keep the video's own
	bool tagsStated = false; // a stated empty list clears; an unstated one keeps the video's
	nlohmann::json tags = nlohmann::json::array();
	std::string language; // empty = keep the video's own
	// True when the user picked the language; false when it is the Windows fallback. A
	// fallback only fills a video that has no language, so it never overrides one set in
	// YouTube Studio or by the channel's upload defaults.
	bool languageExplicit = false;
};

// Everything the edit asks for, reduced to what decides the outgoing body. The skip-if-
// unchanged check hashes this rather than the merged body, so it can run BEFORE the read
// and spend nothing when the edit is one already applied.
nlohmann::json EditDigestSource(const Edit &edit);

// `current` is the video's snippet as videos.list returned it, or an empty object when it
// could not be read. Only writable properties are carried over; a read-only one in a PUT
// is ignored at best and rejected at worst.
nlohmann::json Merge(const nlohmann::json &current, const Edit &edit);

// A Windows language name ("en-US", "zh-Hant-TW") as the language code a video snippet
// takes. The primary subtag alone, which is how YouTube itself stores a detected language
// ("ru", "en"); Chinese keeps its script, because "zh" alone does not say which one the
// viewer reads. Empty when there is no usable language: empty in, a private-use or
// pseudo-locale name ("x-...", "qps-ploc"), or anything that is not a 2-3 letter code.
std::string LanguageFromLocale(const std::string &localeName);

} // namespace YouTubeSnippet

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_YOUTUBE_SNIPPET_HPP_
