#include "twitch_provider.hpp"

#include <array>

#include "../chat/twitch_chat.hpp"
#include "../events/twitch_events.hpp"
#include "util/http_client.hpp"
#include "util/json_util.hpp"
#include "provider_creds.hpp"
#include "ui-config.h"

namespace OAuth {

namespace {

const char *kHelixBase = "https://api.twitch.tv/helix/";

// The scope set the broker requests for Twitch. channel:read:stream_key backs the
// stream-key autofill; channel:manage:broadcast backs the title/category PATCH.
// User identity (GET /helix/users) needs no special scope. chat:read + chat:edit
// back the Phase 9.0 multichat IRC-over-WebSocket read + send. moderator:read:followers,
// channel:read:subscriptions, and bits:read back the Phase 9.2b EventSub feed
// (follower backfill + follow/sub/resub/gift + cheer notifications; channel.raid
// needs no scope). Verified against dev.twitch.tv (2026-07).
const std::array<const char *, 7> kTwitchScopes = {"channel:read:stream_key",
						   "channel:manage:broadcast",
						   "chat:read",
						   "chat:edit",
						   "moderator:read:followers",
						   "channel:read:subscriptions",
						   "bits:read"};

// Twitch's settable content-classification label ids (the PATCH-writable set;
// "MatureGame" is auto-derived from the game rating and is NOT settable here).
// Verified against the Modify Channel Information reference (2026-06).
struct LabelOption {
	const char *id;
	const char *label;
};
const std::array<LabelOption, 6> kContentLabels = {{
	{"DebatedSocialIssuesAndPolitics", "Politics and Sensitive Social Issues"},
	{"DrugsIntoxication", "Drugs, Intoxication, or Excessive Tobacco Use"},
	{"Gambling", "Gambling"},
	{"ProfanityVulgarity", "Significant Profanity or Vulgarity"},
	{"SexualThemes", "Sexual Themes"},
	{"ViolentGraphic", "Violent and Graphic Depictions"},
}};

// A pragmatic subset of Twitch broadcast languages (ISO 639-1 code -> display
// name) for the advanced language enum. Twitch accepts any ISO 639-1 code; this
// covers the common streaming languages.
struct LangOption {
	const char *value;
	const char *label;
};
const std::array<LangOption, 24> kLanguages = {{
	{"en", "English"},    {"es", "Spanish"}, {"fr", "French"},    {"de", "German"},     {"it", "Italian"},
	{"pt", "Portuguese"}, {"ru", "Russian"}, {"ja", "Japanese"},  {"ko", "Korean"},     {"zh", "Chinese"},
	{"nl", "Dutch"},      {"pl", "Polish"},  {"tr", "Turkish"},   {"ar", "Arabic"},     {"cs", "Czech"},
	{"da", "Danish"},     {"fi", "Finnish"}, {"el", "Greek"},     {"hu", "Hungarian"},  {"no", "Norwegian"},
	{"sv", "Swedish"},    {"th", "Thai"},    {"uk", "Ukrainian"}, {"vi", "Vietnamese"},
}};

using JsonUtil::ParseJson;
using JsonUtil::Str;

// The first element of `j["data"]`, or a null json when absent/empty.
json FirstDataRow(const json &j)
{
	if (!j.is_object()) {
		return json(nullptr);
	}
	auto it = j.find("data");
	if (it == j.end() || !it->is_array() || it->empty()) {
		return json(nullptr);
	}
	return (*it)[0];
}

// Validate one Twitch tag: lowercase alphanumeric, no spaces, 1..25 chars.
bool TagValid(const std::string &tag)
{
	if (tag.empty() || tag.size() > 25) {
		return false;
	}
	for (const char c : tag) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (!ok) {
			return false;
		}
	}
	return true;
}

} // namespace

TwitchProvider::TwitchProvider()
	: auth_(BrokerStrategy::Config{
		  BRAIDCAST_BROKER_URL, // brokerBaseUrl
		  "twitch",             // platform
		  TWITCH_SCOPE_VERSION, // scopeVer
	  })
{
}

std::unique_ptr<Chat::ChatTransport> TwitchProvider::makeChat(const OAuthAccount &acct)
{
	(void)acct; // Twitch chat resolves its channel from chatChannelRef(acct) at connect
	// TwitchChat captures &auth_ for its reactive token refresh, so it shares this
	// provider's single strategy (the token store is the real shared state).
	return std::make_unique<TwitchChat>(&auth_);
}

std::unique_ptr<Events::EventTransport> TwitchProvider::makeEvents(const OAuthAccount &acct)
{
	(void)acct; // the EventSub transport reads acct fresh per call via SendAuthed
	// TwitchEvents stores only the provider pointer (this) at construction.
	return std::make_unique<Events::TwitchEvents>(this);
}

json TwitchProvider::capabilityJson() const
{
	json scopes = json::array();
	for (const char *s : kTwitchScopes) {
		scopes.push_back(s);
	}

	json labelOptions = json::array();
	for (const LabelOption &l : kContentLabels) {
		labelOptions.push_back(json{{"value", l.id}, {"label", l.label}});
	}

	json langOptions = json::array();
	for (const LangOption &l : kLanguages) {
		langOptions.push_back(json{{"value", l.value}, {"label", l.label}});
	}

	json fields = json::array();
	fields.push_back(json{{"key", "title"},
			      {"label", "Title"},
			      {"type", "text"},
			      {"tier", "simple"},
			      {"shareable", true},
			      {"max", 140}});
	fields.push_back(json{{"key", "category"},
			      {"label", "Category"},
			      {"type", "category"},
			      {"tier", "simple"},
			      {"shareable", false}});
	fields.push_back(json{{"key", "tags"},
			      {"label", "Tags"},
			      {"type", "tags"},
			      {"tier", "simple"},
			      {"shareable", true},
			      {"max", 10},
			      {"constraint", "lowercase-alnum ≤25"}});
	fields.push_back(json{{"key", "language"},
			      {"label", "Language"},
			      {"type", "enum"},
			      {"tier", "advanced"},
			      {"shareable", false},
			      {"options", langOptions}});
	fields.push_back(json{{"key", "contentLabels"},
			      {"label", "Content Classification"},
			      {"type", "labelset"},
			      {"tier", "advanced"},
			      {"shareable", false},
			      {"options", labelOptions}});
	fields.push_back(json{{"key", "brandedContent"},
			      {"label", "Branded Content"},
			      {"type", "bool"},
			      {"tier", "advanced"},
			      {"shareable", false}});

	return json{
		{"id", id()},
		{"displayName", displayName()},
		{"brandColor", brandColor()},
		{"auth", json{{"strategy", "broker"}, {"scopes", scopes}, {"needsSecret", false}}},
		{"fields", fields},
	};
}

void TwitchProvider::stampAuth(Http::HttpReq &r, const OAuthAccount &acct) const
{
	r.headers.push_back("Client-Id: " + TwitchClientId());
	r.headers.push_back("Authorization: Bearer " + acct.access);
}

bool TwitchProvider::fetchIdentity(OAuthAccount &acct, std::string &err)
{
	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kHelixBase) + "users";

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status == 403) {
		err = "Twitch requires two-factor auth on your account to manage the channel";
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "Twitch users request failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	const json row = FirstDataRow(ParseJson(resp.body));
	if (!row.is_object()) {
		err = "Twitch users response missing data";
		return false;
	}
	acct.userId = Str(row, "id");
	acct.login = Str(row, "login");
	acct.displayName = Str(row, "display_name");
	if (acct.displayName.empty()) {
		acct.displayName = acct.login;
	}
	acct.avatarUrl = Str(row, "profile_image_url");
	if (acct.userId.empty()) {
		err = "Twitch users response missing user id";
		return false;
	}
	return true;
}

bool TwitchProvider::getMetadata(OAuthAccount &acct, json &out, std::string &err)
{
	if (!ensureIdentity(acct, err)) {
		return false;
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kHelixBase) + "channels?broadcaster_id=" + Http::UrlEncode(acct.userId);

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "Twitch channels request failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	const json row = FirstDataRow(ParseJson(resp.body));
	if (!row.is_object()) {
		err = "Twitch channels response missing data";
		return false;
	}
	out = json{
		{"title", Str(row, "title")},
		{"category", json{{"id", Str(row, "game_id")}, {"name", Str(row, "game_name")}}},
		{"language", Str(row, "broadcaster_language")},
	};
	return true;
}

bool TwitchProvider::searchCategories(OAuthAccount &acct, const std::string &query, json &out, std::string &err)
{
	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kHelixBase) + "search/categories?query=" + Http::UrlEncode(query) + "&first=10";

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "Twitch category search failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	const json j = ParseJson(resp.body);
	out = json::array();
	if (j.is_object()) {
		auto it = j.find("data");
		if (it != j.end() && it->is_array()) {
			for (const json &row : *it) {
				out.push_back(json{
					{"id", Str(row, "id")},
					{"name", Str(row, "name")},
					{"boxArt", Str(row, "box_art_url")},
				});
			}
		}
	}
	return true;
}

bool TwitchProvider::applyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields,
				   bool goingLive, std::string &err)
{
	(void)profileUuid; // Twitch edits a persistent channel; no per-profile ingest writeback
	(void)goingLive;   // persistent channel: edit is intent-agnostic, same push whether or not going live
	if (!ensureIdentity(acct, err)) {
		return false;
	}
	if (!fields.is_object()) {
		err = "stream metadata fields must be an object";
		return false;
	}

	json body = json::object();

	// Title: empty is invalid on Twitch -> skip rather than send "".
	if (fields.contains("title") && fields["title"].is_string()) {
		const std::string title = fields["title"].get<std::string>();
		if (!title.empty()) {
			body["title"] = title;
		}
	}

	// Category: only send game_id when a category is actually chosen (clearing it
	// with an empty id is rejected by Twitch).
	if (fields.contains("category") && fields["category"].is_object()) {
		const std::string gameId = Str(fields["category"], "id");
		if (!gameId.empty()) {
			body["game_id"] = gameId;
		}
	}

	if (fields.contains("language") && fields["language"].is_string()) {
		const std::string lang = fields["language"].get<std::string>();
		if (!lang.empty()) {
			body["broadcaster_language"] = lang;
		}
	}

	// Tags: skip empty entries (a stray empty tag must not reject the whole patch);
	// validate the rest and cap the kept set at 10.
	if (fields.contains("tags") && fields["tags"].is_array()) {
		const json &tagsIn = fields["tags"];
		json tags = json::array();
		for (const json &t : tagsIn) {
			if (!t.is_string()) {
				err = "tags must be strings";
				return false;
			}
			const std::string tag = t.get<std::string>();
			if (tag.empty()) {
				continue;
			}
			if (!TagValid(tag)) {
				err = "invalid tag '" + tag +
				      "': tags must be lowercase alphanumeric, no spaces, 25 chars max";
				return false;
			}
			tags.push_back(tag);
		}
		if (tags.size() > 10) {
			err = "Twitch allows at most 10 tags";
			return false;
		}
		body["tags"] = std::move(tags);
	}

	// Content classification labels: the modal submits the labelset as an array of
	// selected `value` strings (the label ids). Mirror that selection across the full
	// known label set so unselected labels are explicitly disabled -- sending only the
	// enabled ones would leave previously-set labels untouched and make clearing
	// impossible.
	if (fields.contains("contentLabels") && fields["contentLabels"].is_array()) {
		std::array<bool, kContentLabels.size()> selected{};
		for (const json &l : fields["contentLabels"]) {
			std::string lid;
			if (l.is_string()) {
				lid = l.get<std::string>();
			} else if (l.is_object()) {
				// Tolerate object entries keyed by `value` (or legacy `id`).
				lid = Str(l, "value");
				if (lid.empty()) {
					lid = Str(l, "id");
				}
			}
			if (lid.empty()) {
				continue;
			}
			for (size_t i = 0; i < kContentLabels.size(); ++i) {
				if (lid == kContentLabels[i].id) {
					selected[i] = true;
					break;
				}
			}
		}
		json labels = json::array();
		for (size_t i = 0; i < kContentLabels.size(); ++i) {
			labels.push_back(json{{"id", kContentLabels[i].id}, {"is_enabled", selected[i]}});
		}
		body["content_classification_labels"] = std::move(labels);
	}

	if (fields.contains("brandedContent") && fields["brandedContent"].is_boolean()) {
		body["is_branded_content"] = fields["brandedContent"].get<bool>();
	}

	if (body.empty()) {
		// Nothing to push -- treat as a no-op success.
		return true;
	}

	Http::HttpReq req;
	req.method = "PATCH";
	req.url = std::string(kHelixBase) + "channels?broadcaster_id=" + Http::UrlEncode(acct.userId);
	req.contentType = "application/json";
	req.body = body.dump();

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	// Success is 204 No Content; accept any 2xx and do not parse a body.
	if (resp.status < 200 || resp.status >= 300) {
		err = "Twitch channel update failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}
	return true;
}

bool TwitchProvider::fetchStreamKey(OAuthAccount &acct, std::string &key, std::string &err)
{
	key.clear();
	if (!ensureIdentity(acct, err)) {
		return false;
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kHelixBase) + "streams/key?broadcaster_id=" + Http::UrlEncode(acct.userId);

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "Twitch stream-key request failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	const json row = FirstDataRow(ParseJson(resp.body));
	key = Str(row, "stream_key");
	if (key.empty()) {
		err = "Twitch stream-key response missing key";
		return false;
	}
	return true;
}

bool TwitchProvider::viewerCount(OAuthAccount &acct, int &out, std::string &err)
{
	out = 0;
	if (!ensureIdentity(acct, err)) {
		return false;
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kHelixBase) + "streams?user_id=" + Http::UrlEncode(acct.userId);

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "Twitch streams request failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	// No data row -> the channel is offline, which is a usable read of 0 viewers.
	const json row = FirstDataRow(ParseJson(resp.body));
	if (row.is_object()) {
		auto it = row.find("viewer_count");
		if (it != row.end() && it->is_number()) {
			out = it->get<int>();
		}
	}
	return true;
}

bool TwitchProvider::audienceCount(OAuthAccount &acct, AudienceResult &out, std::string &err)
{
	if (!ensureIdentity(acct, err)) {
		return false;
	}

	Http::HttpReq req;
	req.method = "GET";
	// `total` is returned regardless of the page size; first=1 keeps the payload minimal.
	req.url = std::string(kHelixBase) + "channels/followers?broadcaster_id=" + Http::UrlEncode(acct.userId) +
		  "&first=1";

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "Twitch followers request failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	const json body = ParseJson(resp.body);
	if (!body.is_object()) {
		err = "Twitch followers response was not a JSON object";
		return false;
	}
	// Guard against a malformed body where `total` is present but non-integer
	// (null/string) -- a bare value<>() would throw type_error.302 on such input.
	auto it = body.find("total");
	if (it != body.end() && it->is_number_integer()) {
		out.count = it->get<int64_t>();
	}
	out.kind = AudienceKind::Followers;
	out.hidden = false;
	out.available = out.count >= 0;
	return out.available;
}

} // namespace OAuth
