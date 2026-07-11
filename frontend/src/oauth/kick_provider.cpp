#include "kick_provider.hpp"

#include <array>

#include "../chat/kick_chat.hpp"
#include "../events/kick_events.hpp"
#include "../http_client.hpp"
#include "../json_util.hpp"
#include "ui-config.h"

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

using JsonUtil::NumLoose;
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

} // namespace

KickProvider::KickProvider()
	: auth_(BrokerStrategy::Config{
		  BRAIDCAST_BROKER_URL, // brokerBaseUrl
		  "kick",               // platform
		  KICK_SCOPE_VERSION,   // scopeVer
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
		{"auth", json{{"strategy", "broker"}, {"scopes", scopes}, {"needsSecret", false}}},
		{"fields", fields},
	};
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
	const int64_t uid = NumLoose(row, "user_id");
	if (uid <= 0) {
		err = "Kick users response missing user id";
		return false;
	}
	acct.userId = std::to_string(uid);
	acct.login = Str(row, "name");
	acct.displayName = acct.login;
	acct.avatarUrl = Str(row, "profile_picture");
	return true;
}

bool KickProvider::getMetadata(OAuthAccount &acct, json &out, std::string &err)
{
	// The channels lookup keys on the user_id, so ensure identity is resolved first.
	if (!ensureIdentity(acct, err)) {
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
		category = json{{"id", std::to_string(NumLoose(cat, "id"))}, {"name", Str(cat, "name")}};
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
	req.url = std::string(kKickApiBase) + "/public/v2/categories?name=" + Http::UrlEncode(query);

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
					{"id", std::to_string(NumLoose(row, "id"))},
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

	// Category: the wire id is a string; NumLoose() parses the numeric string back to
	// the integer category_id Kick's PATCH expects. Send it only when one is actually
	// chosen (a zero/empty id would otherwise clear or reject the field).
	if (fields.contains("category") && fields["category"].is_object()) {
		const int64_t catId = NumLoose(fields["category"], "id");
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
	if (!ensureIdentity(acct, err)) {
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
		out = static_cast<int>(NumLoose(row["stream"], "viewer_count"));
	}
	return true;
}

bool KickProvider::audienceCount(OAuthAccount &acct, AudienceResult &out, std::string &err)
{
	// Kick exposes no REST follower total; a later task pushes the live count instead.
	(void)acct;
	(void)err;
	out.available = false;
	out.kind = AudienceKind::Followers;
	return false;
}

} // namespace OAuth
