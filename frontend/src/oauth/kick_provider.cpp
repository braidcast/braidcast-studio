#include "kick_provider.hpp"

#include <array>

#include "../chat/kick_chat.hpp"
#include "../events/kick_events.hpp"
#include "../http_client.hpp"
#include "../provider_creds.hpp"

namespace OAuth {

namespace {

// Kick's public API base + OAuth endpoints. Kick's OAuth frontend rewrites a
// literal 127.0.0.1 redirect host, so the loopback redirect_uri must advertise
// "localhost" (still resolves to the loopback listener). Verified against
// docs.kick.com (2026-06).
const char *kKickApiBase = "https://api.kick.com";

// channel:read/write back the title/category/tags read + PATCH; user:read backs
// the identity lookup. chat:write backs the Phase 9.0 multichat REST send;
// events:subscribe backs the Pusher chat event subscription. Verified against
// docs.kick.com (2026-06).
const std::array<const char *, 5> kKickScopes = {"channel:read", "channel:write", "user:read",
						 "chat:write",   "events:subscribe"};

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

// Read an integer field tolerantly (Kick ids/user_id are JSON integers; accept a
// numeric string too). Missing/unparseable -> 0.
int Int(const json &j, const char *key)
{
	if (!j.is_object()) {
		return 0;
	}
	auto it = j.find(key);
	if (it == j.end()) {
		return 0;
	}
	if (it->is_number_integer()) {
		return it->get<int>();
	}
	if (it->is_number()) {
		return static_cast<int>(it->get<double>());
	}
	if (it->is_string()) {
		try {
			return std::stoi(it->get<std::string>());
		} catch (const std::exception &) {
			return 0;
		}
	}
	return 0;
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

} // namespace

KickProvider::KickProvider()
	: auth_(PkceLoopbackStrategy::Config{
		  "https://id.kick.com/oauth/authorize",        // authorizeUrl
		  "https://id.kick.com/oauth/token",            // tokenUrl
		  KickClientId(),                               // clientId
		  KickClientSecret(),                           // clientSecret (Kick is confidential)
		  {kKickScopes.begin(), kKickScopes.end()},     // scopes
		  KICK_SCOPE_VERSION,                           // scopeVer
		  "/callback",                                  // redirectPath
		  "localhost",                                  // redirectHost (Kick rewrites 127.0.0.1)
	  })
{
}

std::unique_ptr<Chat::ChatTransport> KickProvider::makeChat(const OAuthAccount &acct)
{
	(void)acct; // KickChat resolves the chatroom id from acct.login at connect
	// KickChat reaches back through this provider for SendAuthed (token coherence).
	return std::make_unique<Chat::KickChat>(*this);
}

std::unique_ptr<Events::EventTransport> KickProvider::makeEvents(const OAuthAccount &acct)
{
	(void)acct; // KickEvents reads acct fresh per call via SendAuthed
	// KickEvents stores only the provider pointer (this) at construction.
	return std::make_unique<Events::KickEvents>(this);
}

json KickProvider::capabilityJson() const
{
	json scopes = json::array();
	for (const char *s : kKickScopes) {
		scopes.push_back(s);
	}

	// Kick has NO advanced fields -- this descriptor drives the modal's
	// empty-advanced "nothing extra to show" path. Title has no documented length
	// cap (the API enforces its own), so `max` is omitted.
	json fields = json::array();
	fields.push_back(json{{"key", "title"}, {"label", "Title"}, {"type", "text"}, {"tier", "simple"}, {"shareable", true}});
	fields.push_back(json{
		{"key", "category"}, {"label", "Category"}, {"type", "category"}, {"tier", "simple"}, {"shareable", false}});
	fields.push_back(json{{"key", "tags"},
			      {"label", "Tags"},
			      {"type", "tags"},
			      {"tier", "simple"},
			      {"shareable", true},
			      {"max", 10}});

	return json{
		{"id", id()},
		{"displayName", displayName()},
		{"brandColor", brandColor()},
		{"auth", json{{"strategy", "pkce-loopback"}, {"scopes", scopes}, {"needsSecret", true}}},
		{"fields", fields},
	};
}

bool KickProvider::SendAuthed(OAuthAccount &acct, Http::HttpReq req, Http::HttpResponse &resp, std::string &err)
{
	// Proactive refresh inside the skew window (best-effort: if it fails the token
	// may still be valid, so we let the request proceed and rely on the 401 path).
	std::string freshErr;
	auth_.ensureFresh(acct, freshErr);

	auto stamp = [&](Http::HttpReq &r) { r.headers.push_back("Authorization: Bearer " + acct.access); };

	Http::HttpReq attempt = req;
	stamp(attempt);
	resp = Http::HttpRequest(attempt);
	if (resp.status == 0) {
		err = "Kick request failed: " + resp.error;
		return false;
	}
	if (resp.status != 401) {
		return true;
	}

	// Reactive 401: force one refresh + retry with the new bearer. Route through
	// ensureFresh(force) -- NOT a bare refresh() -- so Kick's rotated refresh token
	// is re-read + written back under the same single-flight lock the proactive path
	// uses (a bare refresh() would rotate in memory and drop the new token, bricking
	// the account on the next refresh).
	std::string refreshErr;
	if (!auth_.ensureFresh(acct, refreshErr, /*force=*/true)) {
		err = "re-authentication required";
		return false;
	}
	Http::HttpReq retry = req;
	stamp(retry);
	resp = Http::HttpRequest(retry);
	if (resp.status == 0) {
		err = "Kick request failed: " + resp.error;
		return false;
	}
	if (resp.status == 401) {
		err = "re-authentication required";
		return false;
	}
	return true;
}

bool KickProvider::fetchIdentity(OAuthAccount &acct, std::string &err)
{
	// No id param -> the authenticated user.
	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kKickApiBase) + "/public/v1/users";

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "Kick users request failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	const json row = FirstDataRow(ParseJson(resp.body));
	if (!row.is_object()) {
		err = "Kick users response missing data";
		return false;
	}
	const int uid = Int(row, "user_id");
	if (uid <= 0) {
		err = "Kick users response missing user id";
		return false;
	}
	acct.userId = std::to_string(uid);
	acct.login = Str(row, "name");
	acct.displayName = acct.login;

	// The avatar lives on the channels payload, not the users payload, so issue one
	// extra authed read against the same endpoint viewerCount uses. This is best-effort:
	// a failed or empty read must not break connect (a monogram fallback covers it).
	Http::HttpReq chReq;
	chReq.method = "GET";
	chReq.url = std::string(kKickApiBase) + "/public/v1/channels?broadcaster_user_id[]=" + Http::UrlEncode(acct.userId);
	Http::HttpResponse chResp;
	std::string chErr;
	if (SendAuthed(acct, chReq, chResp, chErr) && chResp.status >= 200 && chResp.status < 300) {
		const json chRow = FirstDataRow(ParseJson(chResp.body));
		if (chRow.is_object()) {
			for (const char *key : {"profile_picture", "banner_picture"}) {
				acct.avatarUrl = Str(chRow, key);
				if (!acct.avatarUrl.empty()) {
					break;
				}
			}
			if (acct.avatarUrl.empty() && chRow.contains("user") && chRow["user"].is_object()) {
				acct.avatarUrl = Str(chRow["user"], "profile_picture");
			}
		}
	}
	return true;
}

bool KickProvider::getMetadata(OAuthAccount &acct, json &out, std::string &err)
{
	// The channels lookup keys on the user_id, so ensure identity is resolved first.
	if (acct.userId.empty() && !fetchIdentity(acct, err)) {
		return false;
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kKickApiBase) + "/public/v1/channels?broadcaster_user_id[]=" + Http::UrlEncode(acct.userId);

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "Kick channels request failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	const json row = FirstDataRow(ParseJson(resp.body));
	if (!row.is_object()) {
		err = "Kick channels response missing data";
		return false;
	}

	// Kick's category id is a wire integer, but the cross-provider contract models
	// category id as a STRING (Twitch's game_id is a string), so stringify it here.
	// applyMetadata parses it back to the int category_id and only pushes when
	// non-zero, so an unset category ("0") round-trips without clearing.
	json category = json::object();
	if (row.contains("category") && row["category"].is_object()) {
		const json &cat = row["category"];
		category = json{{"id", std::to_string(Int(cat, "id"))}, {"name", Str(cat, "name")}};
	} else {
		category = json{{"id", std::string("0")}, {"name", std::string()}};
	}

	// Tags live under stream.custom_tags (array of strings).
	json tags = json::array();
	if (row.contains("stream") && row["stream"].is_object()) {
		const json &stream = row["stream"];
		auto it = stream.find("custom_tags");
		if (it != stream.end() && it->is_array()) {
			for (const json &t : *it) {
				if (t.is_string()) {
					tags.push_back(t.get<std::string>());
				}
			}
		}
	}

	out = json{
		{"title", Str(row, "stream_title")},
		{"category", category},
		{"tags", tags},
	};
	return true;
}

bool KickProvider::searchCategories(OAuthAccount &acct, const std::string &query, json &out, std::string &err)
{
	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kKickApiBase) + "/public/v2/categories?name[]=" + Http::UrlEncode(query);

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "Kick category search failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	const json j = ParseJson(resp.body);
	out = json::array();
	if (j.is_object()) {
		auto it = j.find("data");
		if (it != j.end() && it->is_array()) {
			for (const json &row : *it) {
				// Stringify id for cross-provider wire consistency (see getMetadata).
				out.push_back(json{
					{"id", std::to_string(Int(row, "id"))},
					{"name", Str(row, "name")},
					{"boxArt", Str(row, "thumbnail")},
				});
			}
		}
	}
	return true;
}

bool KickProvider::applyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields,
				 std::string &err)
{
	(void)profileUuid; // Kick edits a persistent channel; no per-profile ingest writeback
	if (!fields.is_object()) {
		err = "stream metadata fields must be an object";
		return false;
	}

	json body = json::object();

	// Title: empty is invalid -> skip rather than send "".
	if (fields.contains("title") && fields["title"].is_string()) {
		const std::string title = fields["title"].get<std::string>();
		if (!title.empty()) {
			body["stream_title"] = title;
		}
	}

	// Category: the wire id is a string; Int() parses the numeric string back to the
	// integer category_id Kick's PATCH expects. Send it only when one is actually
	// chosen (a zero/empty id would otherwise clear or reject the field).
	if (fields.contains("category") && fields["category"].is_object()) {
		const int catId = Int(fields["category"], "id");
		if (catId > 0) {
			body["category_id"] = catId;
		}
	}

	// Tags: skip empty entries; cap the kept set at 10 (Kick's max).
	if (fields.contains("tags") && fields["tags"].is_array()) {
		json tags = json::array();
		for (const json &t : fields["tags"]) {
			if (!t.is_string()) {
				err = "tags must be strings";
				return false;
			}
			const std::string tag = t.get<std::string>();
			if (tag.empty()) {
				continue;
			}
			tags.push_back(tag);
		}
		if (tags.size() > 10) {
			err = "Kick allows at most 10 tags";
			return false;
		}
		body["custom_tags"] = std::move(tags);
	}

	if (body.empty()) {
		// Nothing to push -- treat as a no-op success.
		return true;
	}

	Http::HttpReq req;
	req.method = "PATCH";
	req.url = std::string(kKickApiBase) + "/public/v1/channels";
	req.contentType = "application/json";
	req.body = body.dump();

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	// Success is 204 No Content; accept any 2xx and do not parse a body.
	if (resp.status < 200 || resp.status >= 300) {
		err = "Kick channel update failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}
	return true;
}

bool KickProvider::viewerCount(OAuthAccount &acct, int &out, std::string &err)
{
	out = 0;

	// The channels lookup keys on the user_id, so ensure identity is resolved first.
	if (acct.userId.empty() && !fetchIdentity(acct, err)) {
		return false;
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kKickApiBase) + "/public/v1/channels?broadcaster_user_id[]=" + Http::UrlEncode(acct.userId);

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "Kick channels request failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	// stream.viewer_count carries the live count; when the channel is offline the
	// stream object is absent -> a usable read of 0 viewers.
	const json row = FirstDataRow(ParseJson(resp.body));
	if (row.is_object() && row.contains("stream") && row["stream"].is_object()) {
		out = Int(row["stream"], "viewer_count");
	}
	return true;
}

} // namespace OAuth
