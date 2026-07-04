#include "youtube_provider.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iterator>

#include "../chat/youtube_chat.hpp"
#include "../http_client.hpp"
#include "../ingest_writeback.hpp"
#include "../log.hpp"
#include "../provider_creds.hpp"

namespace OAuth {

namespace {

// YouTube Data API v3 endpoints. The live lifecycle creates a fresh broadcast +
// stream per Go Live (create-per-go-live), binds them, then tags the resulting
// video. Verified against the YouTube Data API v3 reference (2026-06).
// Note: create-per-go-live can leave an orphaned broadcast/stream in the user's
// account if a CRITICAL step fails after the insert -- inherent to the model, not
// a leak (no live resource is held; the stale entities are just unused).
const char *kChannelsUrl = "https://www.googleapis.com/youtube/v3/channels";
const char *kVideoCategoriesUrl = "https://www.googleapis.com/youtube/v3/videoCategories";
const char *kLiveBroadcastsUrl = "https://www.googleapis.com/youtube/v3/liveBroadcasts";
const char *kLiveStreamsUrl = "https://www.googleapis.com/youtube/v3/liveStreams";
const char *kVideosUrl = "https://www.googleapis.com/youtube/v3/videos";
const char *kThumbnailsSetUrl = "https://www.googleapis.com/upload/youtube/v3/thumbnails/set";

// YouTube requires a non-empty broadcast title; fall back to this when the modal
// sends none so liveBroadcasts.insert does not 400.
const char *kDefaultTitle = "Live Stream";

// thumbnails.set rejects uploads over 2 MB; skip oversized files client-side.
const std::streamoff kMaxThumbnailBytes = 2 * 1024 * 1024;

// force-ssl is the single broad write scope covering channels.list,
// liveBroadcasts/liveStreams insert+bind, videos.update, thumbnails.set, and
// videoCategories.list -- no narrower per-call scope is needed.
const char *kYouTubeScope = "https://www.googleapis.com/auth/youtube.force-ssl";

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

// Read a boolean field tolerantly: missing/non-bool -> `fallback`.
bool Bool(const json &j, const char *key, bool fallback)
{
	if (!j.is_object()) {
		return fallback;
	}
	auto it = j.find(key);
	if (it == j.end() || !it->is_boolean()) {
		return fallback;
	}
	return it->get<bool>();
}

// The first element of `j["items"]`, or a null json when absent/empty.
json FirstItem(const json &j)
{
	if (!j.is_object()) {
		return json(nullptr);
	}
	auto it = j.find("items");
	if (it == j.end() || !it->is_array() || it->empty()) {
		return json(nullptr);
	}
	return (*it)[0];
}

// Case-insensitive substring test (an empty needle always matches).
bool ContainsCI(const std::string &haystack, const std::string &needle)
{
	if (needle.empty()) {
		return true;
	}
	const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
				    [](char a, char b) {
					    return std::tolower(static_cast<unsigned char>(a)) ==
						   std::tolower(static_cast<unsigned char>(b));
				    });
	return it != haystack.end();
}

// Current UTC time as an RFC3339 instant (the scheduledStartTime YouTube wants).
std::string NowIso8601Utc()
{
	const std::time_t now = std::time(nullptr);
	std::tm tm{};
	gmtime_s(&tm, &now);
	char buf[32];
	std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
	return std::string(buf);
}

// Sniff an image MIME from the leading magic bytes (thumbnails.set wants the
// real Content-Type on the raw upload). Defaults to image/png when unrecognized.
std::string SniffImageMime(const std::string &bytes)
{
	const unsigned char *b = reinterpret_cast<const unsigned char *>(bytes.data());
	const size_t n = bytes.size();
	if (n >= 4 && b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47) {
		return "image/png";
	}
	if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
		return "image/jpeg";
	}
	if (n >= 2 && b[0] == 0x47 && b[1] == 0x49) {
		return "image/gif";
	}
	if (n >= 2 && b[0] == 0x42 && b[1] == 0x4D) {
		return "image/bmp";
	}
	return "image/png";
}

} // namespace

YouTubeProvider::YouTubeProvider()
	: auth_(PkceLoopbackStrategy::Config{
		  "https://accounts.google.com/o/oauth2/v2/auth", // authorizeUrl
		  "https://oauth2.googleapis.com/token",          // tokenUrl
		  YouTubeClientId(),                              // clientId
		  YouTubeClientSecret(),                          // clientSecret (Google ships a non-confidential one; optional)
		  {kYouTubeScope},                                // scopes
		  YOUTUBE_SCOPE_VERSION,                          // scopeVer
		  "/",                                            // redirectPath
		  "127.0.0.1",                                    // redirectHost (Google desktop loopback ignores the port)
		  {{"access_type", "offline"}, {"prompt", "consent"}}, // extraAuthParams (force a refresh token)
	  })
{
}

// Out-of-line default dtor: the header only forward-declares Chat::YouTubeChat (used
// by makeChat + the friend grant), so the translation unit that destroys the provider
// needs the complete-type context this .cpp provides.
YouTubeProvider::~YouTubeProvider() = default;

std::unique_ptr<Chat::ChatTransport> YouTubeProvider::makeChat(const OAuthAccount &acct)
{
	(void)acct; // YouTubeChat reads chatChannelRef(acct) for the account's liveChatId
	return std::make_unique<Chat::YouTubeChat>(*this);
}

std::unique_ptr<Events::EventTransport> YouTubeProvider::makeEvents(const OAuthAccount &acct)
{
	(void)acct; // YouTubeEvents reads acct fresh per call via SendAuthed
	return std::make_unique<Events::YouTubeEvents>(this);
}

std::string YouTubeProvider::chatChannelRef(const OAuthAccount &acct)
{
	const std::lock_guard<std::mutex> guard(broadcastMutex_);
	auto it = broadcasts_.find(AccountId(acct));
	return it != broadcasts_.end() ? it->second.liveChatId : std::string();
}

void YouTubeProvider::clearActiveBroadcast(const std::string &accountId)
{
	const std::lock_guard<std::mutex> guard(broadcastMutex_);
	broadcasts_.erase(accountId);
}

json YouTubeProvider::capabilityJson() const
{
	json scopes = json::array();
	scopes.push_back(kYouTubeScope);

	json fields = json::array();
	fields.push_back(json{{"key", "title"},
			      {"label", "Title"},
			      {"type", "text"},
			      {"tier", "simple"},
			      {"shareable", true},
			      {"max", 100}});
	fields.push_back(json{
		{"key", "category"}, {"label", "Category"}, {"type", "category"}, {"tier", "simple"}, {"shareable", false}});
	fields.push_back(json{{"key", "tags"},
			      {"label", "Tags"},
			      {"type", "tags"},
			      {"tier", "simple"},
			      {"shareable", true},
			      {"max", 500}});
	fields.push_back(json{
		{"key", "thumbnail"}, {"label", "Thumbnail"}, {"type", "image"}, {"tier", "simple"}, {"shareable", false}});
	fields.push_back(json{{"key", "description"},
			      {"label", "Description"},
			      {"type", "textarea"},
			      {"tier", "simple"},
			      {"shareable", true}});
	fields.push_back(json{{"key", "privacy"},
			      {"label", "Privacy"},
			      {"type", "enum"},
			      {"tier", "simple"},
			      {"shareable", false},
			      {"options", json::array({json{{"value", "public"}, {"label", "Public"}},
						       json{{"value", "unlisted"}, {"label", "Unlisted"}},
						       json{{"value", "private"}, {"label", "Private"}}})}});
	fields.push_back(json{{"key", "latency"},
			      {"label", "Latency"},
			      {"type", "enum"},
			      {"tier", "advanced"},
			      {"shareable", false},
			      {"options", json::array({json{{"value", "normal"}, {"label", "Normal"}},
						       json{{"value", "low"}, {"label", "Low latency"}},
						       json{{"value", "ultraLow"}, {"label", "Ultra-low latency"}}})}});
	fields.push_back(json{
		{"key", "dvr"}, {"label", "DVR"}, {"type", "bool"}, {"tier", "advanced"}, {"shareable", false}});
	fields.push_back(json{{"key", "madeForKids"},
			      {"label", "Made for kids"},
			      {"type", "bool"},
			      {"tier", "advanced"},
			      {"shareable", false}});
	fields.push_back(json{{"key", "autoStop"},
			      {"label", "Auto-stop when stream ends"},
			      {"type", "bool"},
			      {"tier", "advanced"},
			      {"shareable", false},
			      {"default", true}});
	fields.push_back(json{{"key", "projection"},
			      {"label", "360\xC2\xB0"},
			      {"type", "bool"},
			      {"tier", "advanced"},
			      {"shareable", false}});

	return json{
		{"id", id()},
		{"displayName", displayName()},
		{"brandColor", brandColor()},
		{"auth", json{{"strategy", "pkce-loopback"}, {"scopes", scopes}, {"needsSecret", false}}},
		{"fields", fields},
	};
}

bool YouTubeProvider::SendAuthed(OAuthAccount &acct, Http::HttpReq req, Http::HttpResponse &resp, std::string &err)
{
	// Proactive refresh inside the skew window (best-effort: if it fails the token
	// may still be valid, so we let the request proceed and rely on the 401 path).
	std::string freshErr;
	auth_.ensureFresh(acct, freshErr);

	// YouTube authenticates with the bearer alone (no Client-Id header like Twitch).
	auto stamp = [&](Http::HttpReq &r) { r.headers.push_back("Authorization: Bearer " + acct.access); };

	Http::HttpReq attempt = req;
	stamp(attempt);
	resp = Http::HttpRequest(attempt);
	if (resp.status == 0) {
		err = "YouTube request failed: " + resp.error;
		return false;
	}
	if (resp.status != 401) {
		return true;
	}

	// Reactive 401: force one refresh + retry with the new bearer. Route through
	// ensureFresh(force) -- NOT a bare refresh() -- so a rotated refresh token is
	// re-read + written back under the same single-flight lock the proactive path
	// uses.
	std::string refreshErr;
	if (!auth_.ensureFresh(acct, refreshErr, /*force=*/true)) {
		err = "re-authentication required";
		return false;
	}
	Http::HttpReq retry = req;
	stamp(retry);
	resp = Http::HttpRequest(retry);
	if (resp.status == 0) {
		err = "YouTube request failed: " + resp.error;
		return false;
	}
	if (resp.status == 401) {
		err = "re-authentication required";
		return false;
	}
	return true;
}

bool YouTubeProvider::fetchIdentity(OAuthAccount &acct, std::string &err)
{
	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kChannelsUrl) + "?part=snippet&mine=true";

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "YouTube channels request failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	const json item = FirstItem(ParseJson(resp.body));
	if (!item.is_object()) {
		err = "YouTube has no channel for this account";
		return false;
	}
	acct.userId = Str(item, "id");
	if (acct.userId.empty()) {
		err = "YouTube channels response missing channel id";
		return false;
	}
	const json snippet = item.contains("snippet") ? item["snippet"] : json(nullptr);
	acct.login = Str(snippet, "title");
	acct.displayName = acct.login;
	return true;
}

bool YouTubeProvider::getMetadata(OAuthAccount &acct, json &out, std::string &err)
{
	// Create-per-go-live: a fresh broadcast is made each time, so there is no
	// meaningful current metadata to prefill. Return an empty object.
	(void)acct;
	(void)err;
	out = json::object();
	return true;
}

bool YouTubeProvider::searchCategories(OAuthAccount &acct, const std::string &query, json &out, std::string &err)
{
	std::vector<std::pair<std::string, std::string>> all;
	{
		const std::lock_guard<std::mutex> guard(categoriesMutex_);
		all = categories_;
	}

	if (all.empty()) {
		Http::HttpReq req;
		req.method = "GET";
		// regionCode=US: every YouTube category is available under US (other regions
		// only ever drop categories), so US + the language part gives the full set.
		req.url = std::string(kVideoCategoriesUrl) + "?part=snippet&regionCode=US&hl=en_US";

		Http::HttpResponse resp;
		if (!SendAuthed(acct, req, resp, err)) {
			return false;
		}
		if (resp.status < 200 || resp.status >= 300) {
			err = "YouTube category list failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
			return false;
		}

		const json j = ParseJson(resp.body);
		if (j.is_object()) {
			auto it = j.find("items");
			if (it != j.end() && it->is_array()) {
				for (const json &row : *it) {
					if (!row.is_object() || !row.contains("snippet") || !row["snippet"].is_object()) {
						continue;
					}
					const json &snippet = row["snippet"];
					if (!Bool(snippet, "assignable", false)) {
						continue;
					}
					const std::string catId = Str(row, "id");
					const std::string name = Str(snippet, "title");
					if (!catId.empty() && !name.empty()) {
						all.emplace_back(catId, name);
					}
				}
			}
		}

		{
			const std::lock_guard<std::mutex> guard(categoriesMutex_);
			categories_ = all;
		}
	}

	out = json::array();
	for (const std::pair<std::string, std::string> &row : all) {
		if (ContainsCI(row.second, query)) {
			out.push_back(json{{"id", row.first}, {"name", row.second}});
		}
	}
	return true;
}

bool YouTubeProvider::applyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields,
				    std::string &err)
{
	if (!fields.is_object()) {
		err = "stream metadata fields must be an object";
		return false;
	}

	// Read every field up front (tolerant defaults; YouTube rejects empties on the
	// required fields, so substitute safe values rather than send "").
	std::string title = Str(fields, "title");
	if (title.empty()) {
		title = kDefaultTitle;
	}
	const std::string description = Str(fields, "description");
	std::string privacy = Str(fields, "privacy");
	if (privacy != "public" && privacy != "unlisted" && privacy != "private") {
		privacy = "public";
	}
	std::string latency = Str(fields, "latency");
	if (latency != "normal" && latency != "low" && latency != "ultraLow") {
		latency = "normal";
	}
	const bool dvr = Bool(fields, "dvr", false);
	const bool madeForKids = Bool(fields, "madeForKids", false);
	const bool autoStop = Bool(fields, "autoStop", true);
	const std::string projection = Bool(fields, "projection", false) ? "360" : "rectangular";

	std::string categoryId;
	if (fields.contains("category") && fields["category"].is_object()) {
		categoryId = Str(fields["category"], "id");
	}

	json tags = json::array();
	if (fields.contains("tags") && fields["tags"].is_array()) {
		for (const json &t : fields["tags"]) {
			if (t.is_string() && !t.get<std::string>().empty()) {
				tags.push_back(t.get<std::string>());
			}
		}
	}

	const std::string thumbnailPath = Str(fields, "thumbnail");

	// One JSON POST/PUT through SendAuthed (so the 401-refresh path covers every
	// step). Fills `outJson` from the response body; `stepErr` carries the reason.
	auto sendJson = [&](const std::string &method, const std::string &url, const json &payload, json &outJson,
			    std::string &stepErr) -> bool {
		Http::HttpReq req;
		req.method = method;
		req.url = url;
		req.contentType = "application/json";
		req.body = payload.dump();
		Http::HttpResponse resp;
		if (!SendAuthed(acct, req, resp, stepErr)) {
			return false;
		}
		if (resp.status < 200 || resp.status >= 300) {
			stepErr = "HTTP " + std::to_string(resp.status) + ": " + resp.body;
			return false;
		}
		outJson = ParseJson(resp.body);
		return true;
	};

	// Best-effort teardown of a broadcast we created but couldn't fully bring up, so a
	// failed (re-)apply doesn't leave an orphan liveBroadcast on the channel. Errors here
	// are logged and swallowed -- they must not mask the original failure that triggered it.
	auto deleteOrphanBroadcast = [&](const std::string &id) {
		if (id.empty()) {
			return;
		}
		Http::HttpReq req;
		req.method = "DELETE";
		req.url = std::string(kLiveBroadcastsUrl) + "?id=" + Http::UrlEncode(id);
		Http::HttpResponse resp;
		std::string delErr;
		if (!SendAuthed(acct, req, resp, delErr)) {
			HostLog("[oauth] YouTube orphan liveBroadcasts.delete failed (ignored): " + delErr);
		} else if (resp.status < 200 || resp.status >= 300) {
			HostLog("[oauth] YouTube orphan liveBroadcasts.delete failed (ignored): HTTP " +
				std::to_string(resp.status) + ": " + resp.body);
		}
	};

	// 1. liveBroadcasts.insert -- the broadcast id doubles as the videoId. CRITICAL.
	json snippet = json{{"title", title}, {"description", description}, {"scheduledStartTime", NowIso8601Utc()}};
	json status = json{{"privacyStatus", privacy}, {"selfDeclaredMadeForKids", madeForKids}};
	json contentDetails = json{
		{"latencyPreference", latency},
		// enableAutoStart must stay true until/unless a manual liveBroadcasts.transition
		// hook exists; without one the broadcast would connect the encoder but never go live.
		{"enableAutoStart", true},
		{"enableAutoStop", autoStop},
		{"enableDvr", dvr},
		{"projection", projection},
		{"monitorStream", json{{"enableMonitorStream", false}}},
	};
	json broadcastBody = json{{"snippet", snippet}, {"status", status}, {"contentDetails", contentDetails}};

	json bResp;
	std::string stepErr;
	if (!sendJson("POST", std::string(kLiveBroadcastsUrl) + "?part=snippet,status,contentDetails", broadcastBody,
		      bResp, stepErr)) {
		err = "YouTube liveBroadcasts.insert failed: " + stepErr;
		return false;
	}
	const std::string broadcastId = Str(bResp, "id");
	if (broadcastId.empty()) {
		err = "YouTube liveBroadcasts.insert returned no broadcast id";
		return false;
	}

	// The broadcast's liveChatId is what the chat transport polls. The insert
	// response usually carries it in snippet; if not, fetch it via liveBroadcasts.list
	// (best-effort -- absence just means chat stays disabled for this go-live).
	std::string liveChatId;
	if (bResp.is_object() && bResp.contains("snippet") && bResp["snippet"].is_object()) {
		liveChatId = Str(bResp["snippet"], "liveChatId");
	}
	if (liveChatId.empty()) {
		Http::HttpReq chatReq;
		chatReq.method = "GET";
		chatReq.url = std::string(kLiveBroadcastsUrl) + "?part=snippet&id=" + Http::UrlEncode(broadcastId);
		Http::HttpResponse chatResp;
		std::string chatErr;
		if (SendAuthed(acct, chatReq, chatResp, chatErr) && chatResp.status >= 200 && chatResp.status < 300) {
			const json item = FirstItem(ParseJson(chatResp.body));
			if (item.is_object() && item.contains("snippet") && item["snippet"].is_object()) {
				liveChatId = Str(item["snippet"], "liveChatId");
			}
		}
		if (liveChatId.empty()) {
			HostLog("[oauth] YouTube broadcast has no liveChatId; chat will be unavailable");
		}
	}

	// 2. liveStreams.insert -> the RTMP ingest endpoint + stream key. CRITICAL.
	json streamBody = json{
		{"snippet", json{{"title", title}}},
		{"cdn", json{{"frameRate", "variable"}, {"ingestionType", "rtmp"}, {"resolution", "variable"}}},
		{"contentDetails", json{{"isReusable", false}}},
	};
	json sResp;
	if (!sendJson("POST", std::string(kLiveStreamsUrl) + "?part=snippet,cdn,contentDetails", streamBody, sResp,
		      stepErr)) {
		err = "YouTube liveStreams.insert failed: " + stepErr;
		deleteOrphanBroadcast(broadcastId);
		return false;
	}
	const std::string streamId = Str(sResp, "id");
	std::string ingestionAddress;
	std::string streamName;
	if (sResp.is_object() && sResp.contains("cdn") && sResp["cdn"].is_object()) {
		const json &cdn = sResp["cdn"];
		if (cdn.contains("ingestionInfo") && cdn["ingestionInfo"].is_object()) {
			ingestionAddress = Str(cdn["ingestionInfo"], "ingestionAddress");
			streamName = Str(cdn["ingestionInfo"], "streamName");
		}
	}
	if (streamId.empty() || streamName.empty() || ingestionAddress.empty()) {
		err = "YouTube liveStreams.insert returned no stream key";
		deleteOrphanBroadcast(broadcastId);
		return false;
	}

	// 3. liveBroadcasts.bind -- attach the stream to the broadcast. CRITICAL.
	const std::string bindUrl = std::string(kLiveBroadcastsUrl) + "/bind?id=" + Http::UrlEncode(broadcastId) +
				    "&streamId=" + Http::UrlEncode(streamId) + "&part=id,contentDetails";
	json bindResp;
	if (!sendJson("POST", bindUrl, json::object(), bindResp, stepErr)) {
		err = "YouTube liveBroadcasts.bind failed: " + stepErr;
		deleteOrphanBroadcast(broadcastId);
		return false;
	}

	// The new broadcast now exists and is bound. Only here do we invalidate this
	// account's prior broadcast chat + viewer-count target -- if any earlier step had
	// failed, the previously-live broadcast stays intact rather than being torn down for
	// one that never came up. The new ids are committed once this apply fully succeeds
	// (below).
	{
		const std::lock_guard<std::mutex> guard(broadcastMutex_);
		BroadcastState &bs = broadcasts_[AccountId(acct)];
		bs.liveChatId.clear();
		bs.broadcastId.clear();
	}

	// 4. videos.update -- category + tags live on the video, not the broadcast.
	// part=snippet REPLACES the whole snippet, so title + categoryId must be
	// re-sent or the call 400s / wipes them. Only worth a call when a category was
	// chosen or tags exist; categoryId 24 (Entertainment) is a safe assignable
	// default needed only when tags exist without a chosen category. NON-CRITICAL.
	if (!categoryId.empty() || !tags.empty()) {
		const std::string effectiveCategory = categoryId.empty() ? "24" : categoryId;
		json videoSnippet = json{
			{"title", title},
			{"description", description},
			{"categoryId", effectiveCategory},
			{"tags", tags},
		};
		json videoBody = json{{"id", broadcastId}, {"snippet", videoSnippet}};
		json vResp;
		std::string vErr;
		if (!sendJson("PUT", std::string(kVideosUrl) + "?part=snippet", videoBody, vResp, vErr)) {
			HostLog("[oauth] YouTube videos.update failed (continuing): " + vErr);
		}
	}

	// 5. thumbnails.set -- raw binary upload (image bytes as the body, sniffed
	// Content-Type), NOT multipart. data: URLs are Phase 8e. NON-CRITICAL.
	if (!thumbnailPath.empty() && thumbnailPath.rfind("data:", 0) != 0) {
		std::ifstream file(thumbnailPath, std::ios::binary | std::ios::ate);
		if (!file) {
			HostLog("[oauth] YouTube thumbnail skipped: cannot open " + thumbnailPath);
		} else if (const std::streamoff size = file.tellg(); size > kMaxThumbnailBytes) {
			// Non-fatal: thumbnail is non-critical, so skip oversized files rather than fail.
			HostLog("[oauth] YouTube thumbnail skipped: file exceeds YouTube's 2 MB limit (" +
				std::to_string(static_cast<long long>(size)) + " bytes) " + thumbnailPath);
		} else {
			file.seekg(0, std::ios::beg);
			const std::string bytes((std::istreambuf_iterator<char>(file)),
						std::istreambuf_iterator<char>());
			if (bytes.empty()) {
				HostLog("[oauth] YouTube thumbnail skipped: empty file " + thumbnailPath);
			} else {
				Http::HttpReq req;
				req.method = "POST";
				req.url = std::string(kThumbnailsSetUrl) + "?videoId=" + Http::UrlEncode(broadcastId);
				req.contentType = SniffImageMime(bytes);
				req.body = bytes;
				Http::HttpResponse resp;
				std::string thumbErr;
				if (!SendAuthed(acct, req, resp, thumbErr)) {
					HostLog("[oauth] YouTube thumbnails.set failed (continuing): " + thumbErr);
				} else if (resp.status < 200 || resp.status >= 300) {
					HostLog("[oauth] YouTube thumbnails.set failed (continuing): HTTP " +
						std::to_string(resp.status) + ": " + resp.body);
				}
			}
		}
	} else if (!thumbnailPath.empty()) {
		HostLog("[oauth] YouTube thumbnail skipped: data: URL handling is Phase 8e");
	}

	// 6. Ingest writeback -- put the CDN endpoint + key into the linked profile so
	// the modal's streaming.start streams to YouTube. Blocks on the UI-thread write
	// so the key is present before the caller triggers go-live. CRITICAL.
	if (!WriteIngestToProfile(profileUuid, ingestionAddress, streamName)) {
		err = "failed to write the YouTube ingest endpoint into the stream profile";
		deleteOrphanBroadcast(broadcastId);
		return false;
	}

	// Go-live setup fully succeeded: publish the broadcast's liveChatId (so the chat
	// transport knows which chat to poll) and broadcastId (so the viewer poller can
	// read its concurrentViewers), both started right after by streaming.start.
	{
		const std::lock_guard<std::mutex> guard(broadcastMutex_);
		BroadcastState &bs = broadcasts_[AccountId(acct)];
		bs.liveChatId = liveChatId;
		bs.broadcastId = broadcastId;
	}
	return true;
}

bool YouTubeProvider::viewerCount(OAuthAccount &acct, int &out, std::string &err)
{
	out = 0;

	std::string broadcastId;
	{
		const std::lock_guard<std::mutex> guard(broadcastMutex_);
		auto it = broadcasts_.find(AccountId(acct));
		if (it != broadcasts_.end()) {
			broadcastId = it->second.broadcastId;
		}
	}
	if (broadcastId.empty()) {
		// No active broadcast -> not live; the poller omits YouTube this cycle.
		err.clear();
		return false;
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kVideosUrl) + "?part=liveStreamingDetails&id=" + Http::UrlEncode(broadcastId);

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "YouTube videos request failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	const json item = FirstItem(ParseJson(resp.body));
	if (item.is_object() && item.contains("liveStreamingDetails") && item["liveStreamingDetails"].is_object()) {
		// concurrentViewers is a STRING in the API ("absent" before/after the live
		// window). Parse tolerantly; absent/garbage -> 0.
		const std::string cv = Str(item["liveStreamingDetails"], "concurrentViewers");
		if (!cv.empty()) {
			try {
				out = std::stoi(cv);
			} catch (const std::exception &) {
				out = 0;
			}
		}
	}
	return true;
}

} // namespace OAuth
