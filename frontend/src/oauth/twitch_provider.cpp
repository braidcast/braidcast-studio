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
const std::array<const char *, 7> kTwitchScopes = {
	"channel:read:stream_key",  "channel:manage:broadcast",   "chat:read", "chat:edit",
	"moderator:read:followers", "channel:read:subscriptions", "bits:read"};

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

using JsonUtil::CopyString;
using JsonUtil::CopyStringList;
using JsonUtil::First;
using JsonUtil::Obj;
using JsonUtil::ParseJson;
using JsonUtil::Str;

// Twitch's tag rules, named once because three places state them: the descriptor that
// advertises them to the UI, TagValid, and the applyMetadata check that refuses a bad
// list outright. A UI hinting a limit the refusal does not use is worse than no hint.
constexpr int kMaxTags = 10;
constexpr int kMaxTagChars = 25;

// Validate one Twitch tag: lowercase alphanumeric, no spaces, 1..kMaxTagChars chars.
bool TagValid(const std::string &tag)
{
	if (tag.empty() || tag.size() > static_cast<size_t>(kMaxTagChars)) {
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

// The account's own /helix/channels row -- the ONE request both the prefill read and the go-live
// read-back run. Named and shaped like Kick's sibling so the two persistent-channel platforms
// answer "what does my channel currently say" the same way.
bool FetchOwnChannelRow(TwitchProvider &provider, OAuthAccount &acct, json &row, std::string &err)
{
	if (!provider.ensureIdentity(acct, err)) {
		return false;
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kHelixBase) + "channels?broadcaster_id=" + Http::UrlEncode(acct.userId);

	Http::HttpResponse resp;
	if (!provider.SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (!Http::Require2xx(resp, "Twitch channels request", err)) {
		return false;
	}

	row = First(ParseJson(resp.body), "data");
	if (!row.is_object()) {
		err = "Twitch channels response missing data";
		return false;
	}
	return true;
}

// Every descriptor field the channel row carries, under the descriptor's own keys. The single
// parser: the prefill publishes a subset of this and the read-back compares all of it.
json ChannelFields(const json &row)
{
	json out = json::object();
	CopyString(row, "title", out, "title");
	// The row states a category by carrying game_id: an UNSET game is an empty id, which is a real
	// answer, whereas a row without the key stated nothing about the category at all.
	json category = json::object();
	if (CopyString(row, "game_id", category, "id")) {
		category["name"] = Str(row, "game_name");
		out["category"] = std::move(category);
	}
	CopyString(row, "broadcaster_language", out, "language");
	CopyStringList(row, "tags", out, "tags");
	CopyStringList(row, "content_classification_labels", out, "contentLabels");
	const json &branded = Obj(row, "is_branded_content");
	if (branded.is_boolean()) {
		out["brandedContent"] = branded.get<bool>();
	}
	return out;
}

} // namespace

TwitchProvider::TwitchProvider()
	: auth_(BrokerStrategy::Config{
		  BRAIDCAST_BROKER_URL, // brokerBaseUrl
		  "twitch",             // platform
		  TWITCH_SCOPE_VERSION, // scopeVer
		  true,                 // revokePreferAccessToken -- Twitch's revoke only documents access tokens
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
			      {"scope", "all"},
			      {"max", 140}});
	// Category ids are Helix game ids, meaningless to any other platform, so the value is
	// shared among the user's Twitch channels and no further.
	fields.push_back(json{{"key", "category"},
			      {"label", "Category"},
			      {"type", "category"},
			      {"tier", "simple"},
			      {"scope", "provider"}});
	// Scoped to Twitch for the same reason applyMetadata rejects a bad tag outright: only
	// here is a tag required to be lowercase alphanumeric with no spaces. Kick and YouTube
	// take arbitrary strings, so one value spanning all three would be a value Twitch
	// refuses -- and its refusal fails the whole push, not just the tag.
	fields.push_back(json{{"key", "tags"},
			      {"label", "Tags"},
			      {"type", "tags"},
			      {"tier", "simple"},
			      {"scope", "provider"},
			      {"maxTags", kMaxTags},
			      {"maxTagChars", kMaxTagChars},
			      {"tagCharset", "lowercase-alnum"}});
	fields.push_back(json{{"key", "language"},
			      {"label", "Language"},
			      {"type", "enum"},
			      {"tier", "advanced"},
			      {"scope", "channel"},
			      {"options", langOptions}});
	fields.push_back(json{{"key", "contentLabels"},
			      {"label", "Content Classification"},
			      {"type", "labelset"},
			      {"tier", "advanced"},
			      {"scope", "channel"},
			      {"options", labelOptions}});
	fields.push_back(json{{"key", "brandedContent"},
			      {"label", "Branded Content"},
			      {"type", "bool"},
			      {"tier", "advanced"},
			      {"scope", "channel"}});

	return json{
		{"id", id()},
		{"displayName", displayName()},
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
	if (!Http::Require2xx(resp, "Twitch users request", err)) {
		return false;
	}

	const json row = First(ParseJson(resp.body), "data");
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
	AppliedState channel;
	if (!readAppliedMetadata(acct, std::string(), AppliedBy::Edit, channel, err)) {
		return false;
	}
	// The prefill publishes only the keys the dialog has always seeded from the channel. The
	// wider read-back set exists to be COMPARED against what a go-live asked for, not to
	// overwrite fields the user is editing.
	//
	// Read through the tolerant accessors because the two contracts are opposites: the read-back
	// bag OMITS a field the channel did not state, while the modal renders every key it is given
	// and so needs a full bag.
	const json &category = Obj(channel.fields, "category");
	out = json{{"title", Str(channel.fields, "title")},
		   {"category", json{{"id", Str(category, "id")}, {"name", Str(category, "name")}}},
		   {"language", Str(channel.fields, "language")}};
	return true;
}

bool TwitchProvider::readAppliedMetadata(OAuthAccount &acct, const std::string &profileUuid, AppliedBy by,
					 AppliedState &out, std::string &err)
{
	(void)profileUuid; // one persistent channel per account, whichever profile points at it
	(void)by;          // the channel is the same row whether this go-live edited it or not
	json row;
	if (!FetchOwnChannelRow(*this, acct, row, err)) {
		return false;
	}
	out = AppliedState{};
	out.fields = ChannelFields(row);
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
	if (!Http::Require2xx(resp, "Twitch category search", err)) {
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
	// validate the rest and cap the kept set at kMaxTags.
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
				err = "invalid tag '" + tag + "': tags must be lowercase alphanumeric, no spaces, " +
				      std::to_string(kMaxTagChars) + " chars max";
				return false;
			}
			tags.push_back(tag);
		}
		if (tags.size() > static_cast<size_t>(kMaxTags)) {
			err = "Twitch allows at most " + std::to_string(kMaxTags) + " tags";
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
	if (!Http::Require2xx(resp, "Twitch channel update", err)) {
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
	if (!Http::Require2xx(resp, "Twitch stream-key request", err)) {
		return false;
	}

	const json row = First(ParseJson(resp.body), "data");
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
	if (!Http::Require2xx(resp, "Twitch streams request", err)) {
		return false;
	}

	// No data row -> the channel is offline, which is a usable read of 0 viewers.
	const json row = First(ParseJson(resp.body), "data");
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
	if (!Http::Require2xx(resp, "Twitch followers request", err)) {
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
