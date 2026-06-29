#include "twitch_provider.hpp"

#include <array>

#include "../http_client.hpp"
#include "../provider_creds.hpp"

namespace OAuth {

namespace {

const char *kHelixBase = "https://api.twitch.tv/helix/";

// The scope set the device-code grant requests. channel:read:stream_key backs the
// stream-key autofill; channel:manage:broadcast backs the title/category PATCH.
// User identity (GET /helix/users) needs no special scope. Verified against
// dev.twitch.tv (2026-06).
const std::array<const char *, 2> kTwitchScopes = {"channel:read:stream_key", "channel:manage:broadcast"};

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
	{"en", "English"},   {"es", "Spanish"},   {"fr", "French"},   {"de", "German"},
	{"it", "Italian"},   {"pt", "Portuguese"}, {"ru", "Russian"},  {"ja", "Japanese"},
	{"ko", "Korean"},    {"zh", "Chinese"},   {"nl", "Dutch"},    {"pl", "Polish"},
	{"tr", "Turkish"},   {"ar", "Arabic"},    {"cs", "Czech"},    {"da", "Danish"},
	{"fi", "Finnish"},   {"el", "Greek"},     {"hu", "Hungarian"}, {"no", "Norwegian"},
	{"sv", "Swedish"},   {"th", "Thai"},      {"uk", "Ukrainian"}, {"vi", "Vietnamese"},
}};

// Parse a body into a JSON object, tolerating garbage (returns null on failure).
json ParseJson(const std::string &body)
{
	return json::parse(body, nullptr, false);
}

// Read a string field tolerantly: missing/non-string -> "".
std::string Str(const json &j, const char *key)
{
	if (!j.is_object()) {
		return std::string();
	}
	auto it = j.find(key);
	if (it == j.end() || !it->is_string()) {
		return std::string();
	}
	return it->get<std::string>();
}

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
	: auth_(DeviceCodeStrategy::Config{
		  "https://id.twitch.tv/oauth2/device", // deviceUrl
		  "https://id.twitch.tv/oauth2/token",  // tokenUrl
		  TwitchClientId(),                     // clientId
		  {kTwitchScopes.begin(), kTwitchScopes.end()}, // scopes
		  TWITCH_SCOPE_VERSION,                 // scopeVer
		  "scopes",                             // scopeParam (Twitch names it "scopes")
	  })
{
}

json TwitchProvider::capabilityJson() const
{
	json scopes = json::array();
	for (const char *s : kTwitchScopes) {
		scopes.push_back(s);
	}

	json labelOptions = json::array();
	for (const LabelOption &l : kContentLabels) {
		labelOptions.push_back(json{{"id", l.id}, {"label", l.label}});
	}

	json langOptions = json::array();
	for (const LangOption &l : kLanguages) {
		langOptions.push_back(json{{"value", l.value}, {"label", l.label}});
	}

	json fields = json::array();
	fields.push_back(json{
		{"key", "title"}, {"label", "Title"}, {"type", "text"}, {"tier", "simple"}, {"shareable", true}, {"max", 140}});
	fields.push_back(json{
		{"key", "category"}, {"label", "Category"}, {"type", "category"}, {"tier", "simple"}, {"shareable", false}});
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
		{"auth", json{{"strategy", "device-code"}, {"scopes", scopes}, {"needsSecret", false}}},
		{"fields", fields},
	};
}

bool TwitchProvider::SendAuthed(OAuthAccount &acct, Http::HttpReq req, Http::HttpResponse &resp, std::string &err)
{
	// Proactive refresh inside the skew window (best-effort: if it fails the token
	// may still be valid, so we let the request proceed and rely on the 401 path).
	std::string freshErr;
	auth_.ensureFresh(acct, freshErr);

	const std::string clientId = TwitchClientId();
	auto stamp = [&](Http::HttpReq &r) {
		r.headers.push_back("Client-Id: " + clientId);
		r.headers.push_back("Authorization: Bearer " + acct.access);
	};

	Http::HttpReq attempt = req;
	stamp(attempt);
	resp = Http::HttpRequest(attempt);
	if (resp.status == 0) {
		err = "Twitch request failed: " + resp.error;
		return false;
	}
	if (resp.status != 401) {
		return true;
	}

	// Reactive 401: force one refresh + retry with the new bearer.
	std::string refreshErr;
	if (!auth_.refresh(acct, refreshErr)) {
		err = "re-authentication required";
		return false;
	}
	Http::HttpReq retry = req;
	stamp(retry);
	resp = Http::HttpRequest(retry);
	if (resp.status == 0) {
		err = "Twitch request failed: " + resp.error;
		return false;
	}
	if (resp.status == 401) {
		err = "re-authentication required";
		return false;
	}
	return true;
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
	if (acct.userId.empty()) {
		err = "Twitch users response missing user id";
		return false;
	}
	return true;
}

bool TwitchProvider::getMetadata(OAuthAccount &acct, json &out, std::string &err)
{
	if (acct.userId.empty()) {
		err = "Twitch account identity missing; reconnect the account";
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

bool TwitchProvider::applyMetadata(OAuthAccount &acct, const json &fields, std::string &err)
{
	if (acct.userId.empty()) {
		err = "Twitch account identity missing; reconnect the account";
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

	// Tags: validate up-front; reject the whole request on any invalid tag.
	if (fields.contains("tags") && fields["tags"].is_array()) {
		const json &tagsIn = fields["tags"];
		if (tagsIn.size() > 10) {
			err = "Twitch allows at most 10 tags";
			return false;
		}
		json tags = json::array();
		for (const json &t : tagsIn) {
			if (!t.is_string()) {
				err = "tags must be strings";
				return false;
			}
			const std::string tag = t.get<std::string>();
			if (!TagValid(tag)) {
				err = "invalid tag '" + tag +
				      "': tags must be lowercase alphanumeric, no spaces, 25 chars max";
				return false;
			}
			tags.push_back(tag);
		}
		body["tags"] = std::move(tags);
	}

	// Content classification labels: array of { id, enabled|is_enabled }.
	if (fields.contains("contentLabels") && fields["contentLabels"].is_array()) {
		json labels = json::array();
		for (const json &l : fields["contentLabels"]) {
			if (!l.is_object()) {
				continue;
			}
			const std::string lid = Str(l, "id");
			if (lid.empty()) {
				continue;
			}
			bool enabled = false;
			if (l.contains("is_enabled") && l["is_enabled"].is_boolean()) {
				enabled = l["is_enabled"].get<bool>();
			} else if (l.contains("enabled") && l["enabled"].is_boolean()) {
				enabled = l["enabled"].get<bool>();
			}
			labels.push_back(json{{"id", lid}, {"is_enabled", enabled}});
		}
		if (!labels.empty()) {
			body["content_classification_labels"] = std::move(labels);
		}
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
	if (acct.userId.empty()) {
		err = "Twitch account identity missing; reconnect the account";
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

} // namespace OAuth
