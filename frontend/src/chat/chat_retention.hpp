#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_RETENTION_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_RETENTION_HPP_

#include <optional>
#include <string_view>

// How long chat history is kept on disk, and its General settings token. Apart from
// chat_archive.hpp so the settings struct can name its default without the archive.
namespace Chat {

enum class Retention { Off, Session, SevenDays };

// Off until the owner settles the release gates for keeping chat on disk
// (braidcast-notes/owner-decisions.md, items 1-4). The General setting's default. A stored
// value this build does not know does not fall back to it: the archive then runs as 7 days
// without saving new chat (ChatArchive::Options::unknownSetting).
inline constexpr Retention kDefaultRetention = Retention::Off;

struct RetentionTokenEntry {
	const char *token;
	Retention retention;
};

// The `chatHistoryRetention` wire and general.json values.
inline constexpr RetentionTokenEntry kRetentionTokens[] = {
	{"off", Retention::Off},
	{"session", Retention::Session},
	{"7d", Retention::SevenDays},
};

constexpr const char *RetentionToken(Retention retention)
{
	for (const RetentionTokenEntry &e : kRetentionTokens) {
		if (e.retention == retention) {
			return e.token;
		}
	}
	return kRetentionTokens[0].token;
}

constexpr std::optional<Retention> RetentionFromToken(std::string_view token)
{
	for (const RetentionTokenEntry &e : kRetentionTokens) {
		if (token == e.token) {
			return e.retention;
		}
	}
	return std::nullopt;
}

// Every mode has its own token and reads back as itself, so a table edit cannot make a
// stored value load as a different mode.
constexpr bool RetentionTokensRoundTrip()
{
	for (const RetentionTokenEntry &e : kRetentionTokens) {
		const std::optional<Retention> back = RetentionFromToken(RetentionToken(e.retention));
		if (!back || *back != e.retention || !RetentionFromToken(e.token)) {
			return false;
		}
	}
	return RetentionFromToken(RetentionToken(Retention::Off)) == Retention::Off &&
	       RetentionFromToken(RetentionToken(Retention::Session)) == Retention::Session &&
	       RetentionFromToken(RetentionToken(Retention::SevenDays)) == Retention::SevenDays;
}
static_assert(RetentionTokensRoundTrip(), "kRetentionTokens must give every Retention one token that reads back");

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_RETENTION_HPP_
