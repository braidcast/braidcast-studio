#include "facebook_provider.hpp"

#include <array>
#include <utility>

#include "util/async_task.hpp"
#include "util/http_client.hpp"
#include "../ingest_writeback.hpp"
#include "util/json_util.hpp"
#include "util/string_util.hpp"
#include "../log.hpp"
#include "ui-config.h"

namespace OAuth {

namespace {

// Pinned deliberately. Meta keeps a Graph version working for about two years after its
// successor ships and then removes it, so an unpinned "latest" would silently change
// request and response shapes under an already-shipped build. Moving forward is an
// edit here plus a re-test, never something that happens on its own.
constexpr const char *kGraphVersion = "v25.0";

std::string GraphUrl(const std::string &path)
{
	return std::string("https://graph.facebook.com/") + kGraphVersion + "/" + path;
}

// pages_show_list lists the account's Pages and yields their access tokens;
// pages_read_engagement + pages_manage_posts + publish_video back creating and ending a
// live video on a Page. No user-level publishing scope is requested: this integration is
// Pages-only and never posts as the person.
const std::array<const char *, 4> kFacebookScopes = {"pages_show_list", "pages_read_engagement", "pages_manage_posts",
						     "publish_video"};

// A Page has no per-post friend circles, so "who can see this" reduces to whether the
// broadcast is published to the Page at all -- which Meta expresses through the
// live_videos `status` parameter, not through the user-oriented privacy object. One row
// per offered choice; the field value is ours, the status is the API's.
struct PrivacyOption {
	const char *value;
	const char *label;
	const char *status;
};
const std::array<PrivacyOption, 2> kPrivacyOptions = {{
	{"public", "Public", "LIVE_NOW"},
	{"unpublished", "Unpublished (Page admins only)", "UNPUBLISHED"},
}};

// Facebook rejects a live video whose title exceeds 254 characters.
constexpr int kMaxTitleLength = 254;

using Http::AppendForm;
using JsonUtil::Obj;
using JsonUtil::ParseJson;
using JsonUtil::Str;
using StringUtil::ContainsCI;

const char *StatusForPrivacy(const std::string &value)
{
	for (const PrivacyOption &o : kPrivacyOptions) {
		if (value == o.value) {
			return o.status;
		}
	}
	// The descriptor marks the field required with a default, so an unrecognized value
	// means a stale remembered bag rather than a real choice: publish, matching the
	// default the modal shows.
	return kPrivacyOptions[0].status;
}

// A scratch account carrying ONE Page's access token, so a Page-scoped call rides the
// shared SendAuthed transport instead of hand-rolling its own auth header. A Page token
// is a different credential from the user token the stored record holds, and a copy
// rather than a mutation keeps that record intact. Nothing here can be written back:
// this strategy issues no refresh token, so ensureFresh returns before it would touch
// the account store.
OAuthAccount PageAccount(const std::string &providerId, const std::string &pageToken)
{
	OAuthAccount page;
	page.providerId = providerId;
	page.access = pageToken;
	return page;
}

// Split "rtmps://live-api-s.facebook.com:443/rtmp/<key>" into the server the RTMP output
// connects to and the key it presents. The last '/' is the boundary: everything Meta puts
// after it (including any query string) is the key, and the trailing slash stays on the
// server exactly as the rtmp-services entry writes it.
bool SplitIngestUrl(const std::string &url, std::string &server, std::string &key)
{
	const size_t slash = url.rfind('/');
	if (slash == std::string::npos || slash + 1 >= url.size()) {
		return false;
	}
	server = url.substr(0, slash + 1);
	key = url.substr(slash + 1);
	return true;
}

// The metadata every live_videos write shares (create and mid-stream edit). Empty values
// are omitted rather than sent blank, so an untouched field never clears what is there.
void AppendMetadataFields(std::string &body, const json &fields)
{
	std::string title = Str(fields, "title");
	if (title.size() > static_cast<size_t>(kMaxTitleLength)) {
		title.resize(kMaxTitleLength);
	}
	if (!title.empty()) {
		AppendForm(body, "title", title);
	}
	const std::string description = Str(fields, "description");
	if (!description.empty()) {
		AppendForm(body, "description", description);
	}
}

} // namespace

FacebookProvider::FacebookProvider()
	: auth_(BrokerStrategy::Config{
		  BRAIDCAST_BROKER_URL,   // brokerBaseUrl
		  "facebook",             // platform
		  FACEBOOK_SCOPE_VERSION, // scopeVer
		  true,                   // revokePreferAccessToken -- Meta revokes by the bearer it is given
		  false,                  // usesRefreshToken -- Meta's grant carries none; Page tokens don't expire
		  "fb_exchange_token",    // longLivedGrantType
		  "fb_exchange_token",    // longLivedTokenField
	  })
{
}

json FacebookProvider::capabilityJson() const
{
	json scopes = json::array();
	for (const char *s : kFacebookScopes) {
		scopes.push_back(s);
	}

	json privacyOptions = json::array();
	for (const PrivacyOption &o : kPrivacyOptions) {
		privacyOptions.push_back(json{{"value", o.value}, {"label", o.label}});
	}

	json fields = json::array();
	// The Page picker. `browsable` makes the lookup run on focus with an empty query, so
	// the Pages are a list to pick from rather than something the user has to guess the
	// name of; the options come from the account, which a static descriptor enum cannot
	// express.
	fields.push_back(json{{"key", "page"},
			      {"label", "Page"},
			      {"type", "category"},
			      {"tier", "simple"},
			      {"shareable", false},
			      {"browsable", true},
			      {"placeholder", "Choose a Page\xE2\x80\xA6"}});
	fields.push_back(json{{"key", "title"},
			      {"label", "Title"},
			      {"type", "text"},
			      {"tier", "simple"},
			      {"shareable", true},
			      {"max", kMaxTitleLength}});
	fields.push_back(json{{"key", "description"},
			      {"label", "Description"},
			      {"type", "textarea"},
			      {"tier", "simple"},
			      {"shareable", true}});
	// Required: Facebook rejects an empty status, and the value decides whether the
	// broadcast is visible to the Page's audience at all -- so the control must show the
	// value that will actually be sent rather than an unset dash.
	fields.push_back(json{{"key", "privacy"},
			      {"label", "Privacy"},
			      {"type", "enum"},
			      {"tier", "simple"},
			      {"shareable", false},
			      {"required", true},
			      {"default", kPrivacyOptions[0].value},
			      {"options", privacyOptions}});

	// No category field: Facebook Gaming was sunset and nothing replaced its game
	// catalog. content_tags are generic interest ids, not a browsable taxonomy, so
	// offering a picker over them would be a catalog that does not exist.
	return json{
		{"id", id()},
		{"displayName", displayName()},
		{"auth", json{{"strategy", "broker"}, {"scopes", scopes}, {"needsSecret", false}}},
		{"fields", fields},
	};
}

bool FacebookProvider::fetchIdentity(OAuthAccount &acct, std::string &err)
{
	Http::HttpReq req;
	req.method = "GET";
	req.url = GraphUrl("me?fields=id,name,picture.type(large)");

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (!Http::Require2xx(resp, "Facebook profile request", err)) {
		return false;
	}

	const json j = ParseJson(resp.body);
	acct.userId = Str(j, "id");
	if (acct.userId.empty()) {
		err = "Facebook profile response missing user id";
		return false;
	}
	acct.login = Str(j, "name");
	acct.displayName = acct.login;
	acct.avatarUrl = Str(Obj(Obj(j, "picture"), "data"), "url");
	return true;
}

bool FacebookProvider::FetchPages(OAuthAccount &acct, std::vector<PageRef> &out, std::string &err)
{
	out.clear();

	Http::HttpReq req;
	req.method = "GET";
	// 100 is Meta's per-page maximum for this edge and far beyond what a streamer
	// administers, so a single request answers without paging.
	req.url = GraphUrl("me/accounts?fields=id,name,access_token&limit=100");

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (!Http::Require2xx(resp, "Facebook Pages request", err)) {
		return false;
	}

	const json j = ParseJson(resp.body);
	const json &data = Obj(j, "data");
	if (!data.is_array()) {
		err = "Facebook Pages response missing data";
		return false;
	}
	for (const json &row : data) {
		PageRef page{Str(row, "id"), Str(row, "name"), Str(row, "access_token")};
		// A Page whose token did not come back cannot be streamed to, and one with no id
		// cannot be addressed -- skip rather than offer a choice that would fail later.
		if (page.id.empty() || page.token.empty()) {
			continue;
		}
		if (page.name.empty()) {
			page.name = page.id;
		}
		out.push_back(std::move(page));
	}
	return true;
}

bool FacebookProvider::getMetadata(OAuthAccount &acct, json &out, std::string &err)
{
	out = json::object();
	// Create-per-go-live: a fresh live video is made each time, so there is no title or
	// description to prefill. The Page is the exception -- it is a standing choice, and
	// with exactly one Page there is nothing to choose, so seed it and let the user go
	// live without touching the field.
	std::vector<PageRef> pages;
	if (!FetchPages(acct, pages, err)) {
		return false;
	}
	if (pages.size() == 1) {
		out["page"] = json{{"id", pages[0].id}, {"name", pages[0].name}};
	}
	return true;
}

bool FacebookProvider::searchCategories(OAuthAccount &acct, const std::string &query, json &out, std::string &err)
{
	std::vector<PageRef> pages;
	if (!FetchPages(acct, pages, err)) {
		return false;
	}

	// Read live rather than cached: a Page created or handed over between go-lives has to
	// show up, and the list is one small request.
	out = json::array();
	for (const PageRef &page : pages) {
		if (!ContainsCI(page.name, query)) {
			continue;
		}
		out.push_back(json{{"id", page.id}, {"name", page.name}});
	}
	return true;
}

bool FacebookProvider::ResolvePage(OAuthAccount &acct, const json &fields, PageRef &out, std::string &err)
{
	const std::string chosenId = Str(Obj(fields, "page"), "id");

	std::vector<PageRef> pages;
	if (!FetchPages(acct, pages, err)) {
		return false;
	}
	if (pages.empty()) {
		err = "this Facebook account administers no Pages; Braidcast streams to Pages only";
		return false;
	}
	if (chosenId.empty()) {
		if (pages.size() > 1) {
			err = "choose which Facebook Page to stream to";
			return false;
		}
		out = pages.front();
		return true;
	}
	for (const PageRef &page : pages) {
		if (page.id == chosenId) {
			out = page;
			return true;
		}
	}
	err = "the selected Facebook Page is no longer available to this account; choose it again";
	return false;
}

bool FacebookProvider::ActiveLiveVideo(const DestinationId &dest, LiveVideo &out) const
{
	const std::lock_guard<std::mutex> guard(liveVideoMutex_);
	auto it = liveVideos_.find(dest);
	if (it == liveVideos_.end()) {
		return false;
	}
	out = it->second;
	return true;
}

bool FacebookProvider::applyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields,
				     bool goingLive, std::string &err)
{
	if (!fields.is_object()) {
		err = "stream metadata fields must be an object";
		return false;
	}
	if (!ensureIdentity(acct, err)) {
		return false;
	}
	const DestinationId dest{AccountId(acct), profileUuid};

	if (!goingLive) {
		// A standalone "Edit stream info" push. Under create-per-go-live there is nothing
		// to create here: pre-live the metadata is already remembered client-side and gets
		// applied when Go Live creates the live video. Mid-stream, edit the running one in
		// place -- title/description only, since the ingest endpoint must not move under a
		// connected encoder.
		LiveVideo active;
		if (!ActiveLiveVideo(dest, active)) {
			return true;
		}
		std::string body;
		AppendMetadataFields(body, fields);
		if (body.empty()) {
			return true;
		}

		Http::HttpReq req;
		req.method = "POST";
		req.url = GraphUrl(active.id);
		req.contentType = "application/x-www-form-urlencoded";
		req.body = body;

		OAuthAccount pageAcct = PageAccount(id(), active.pageToken);
		Http::HttpResponse resp;
		if (!SendAuthed(pageAcct, req, resp, err)) {
			return false;
		}
		return Http::Require2xx(resp, "Facebook live-video update", err);
	}

	PageRef page;
	if (!ResolvePage(acct, fields, page, err)) {
		return false;
	}

	std::string body;
	AppendMetadataFields(body, fields);
	AppendForm(body, "status", StatusForPrivacy(Str(fields, "privacy")));

	Http::HttpReq req;
	req.method = "POST";
	req.url = GraphUrl(page.id + "/live_videos");
	req.contentType = "application/x-www-form-urlencoded";
	req.body = body;

	OAuthAccount pageAcct = PageAccount(id(), page.token);
	Http::HttpResponse resp;
	if (!SendAuthed(pageAcct, req, resp, err)) {
		return false;
	}
	if (!Http::Require2xx(resp, "Facebook live-video create", err)) {
		return false;
	}

	const json created = ParseJson(resp.body);
	const std::string liveVideoId = Str(created, "id");
	std::string server;
	std::string key;
	// secure_stream_url is the RTMPS endpoint; the plain stream_url is RTMP and Facebook
	// no longer accepts it, so a response without the secure form is unusable.
	if (liveVideoId.empty() || !SplitIngestUrl(Str(created, "secure_stream_url"), server, key)) {
		err = "Facebook live-video create returned no usable RTMPS ingest endpoint";
		return false;
	}

	const LiveVideo live{liveVideoId, page.token};
	// Ingest writeback -- put the endpoint + key into the linked profile so the modal's
	// streaming.start streams to Facebook. Blocks on the UI-thread write so the key is
	// present before the caller triggers go-live.
	if (!WriteIngestToProfile(profileUuid, server, key)) {
		err = "failed to write the Facebook ingest endpoint into the stream profile";
		EndLiveVideos({live});
		return false;
	}

	// Go-live setup fully succeeded: publish the live video so the stop hooks can end it.
	// Only here, so a failure above leaves any previously-live broadcast on this
	// destination untouched rather than replaced by one that never came up. A go-live that
	// was NOT preceded by a stop supersedes an entry this destination still holds -- end
	// that one rather than drop the handle, which would leave it open on the Page until
	// Meta times it out.
	std::vector<LiveVideo> superseded;
	{
		const std::lock_guard<std::mutex> guard(liveVideoMutex_);
		auto it = liveVideos_.find(dest);
		if (it != liveVideos_.end()) {
			superseded.push_back(it->second);
		}
		liveVideos_[dest] = live;
	}
	EndLiveVideos(std::move(superseded));
	HostLog("[oauth] Facebook live video created for dest=" + DestinationKey(dest) + " on Page " + page.name);
	return true;
}

void FacebookProvider::clearActiveBroadcast(const std::string &accountId)
{
	std::vector<LiveVideo> ending;
	{
		const std::lock_guard<std::mutex> guard(liveVideoMutex_);
		// Every destination of this account, not one entry: the account may hold several
		// concurrent live videos and a stop ends all of them.
		for (auto it = liveVideos_.begin(); it != liveVideos_.end();) {
			if (it->first.accountId != accountId) {
				++it;
				continue;
			}
			ending.push_back(it->second);
			it = liveVideos_.erase(it);
		}
	}
	EndLiveVideos(std::move(ending));
}

void FacebookProvider::clearActiveBroadcastDestination(const DestinationId &dest)
{
	std::vector<LiveVideo> ending;
	{
		const std::lock_guard<std::mutex> guard(liveVideoMutex_);
		auto it = liveVideos_.find(dest);
		if (it != liveVideos_.end()) {
			ending.push_back(it->second);
			liveVideos_.erase(it);
		}
	}
	EndLiveVideos(std::move(ending));
}

void FacebookProvider::EndLiveVideos(std::vector<LiveVideo> ending)
{
	if (ending.empty()) {
		return;
	}
	// Both callers run on the UI thread, so the Graph requests go to a worker. Capturing
	// `this` is safe on the registered-async seam: bridge teardown drains every RunAsync
	// worker before the statics these touch are torn down.
	AsyncTask::RunAsync([this, ending = std::move(ending)] {
		for (const LiveVideo &live : ending) {
			std::string body;
			AppendForm(body, "end_live_video", "true");

			Http::HttpReq req;
			req.method = "POST";
			req.url = GraphUrl(live.id);
			req.contentType = "application/x-www-form-urlencoded";
			req.body = body;

			OAuthAccount pageAcct = PageAccount(id(), live.pageToken);
			Http::HttpResponse resp;
			std::string err;
			// Best-effort by design: the local stop already succeeded and stays
			// succeeded, and Facebook ends an abandoned live video itself once ingest
			// stops. Log the outcome (never a token) and move on.
			if (!SendAuthed(pageAcct, req, resp, err)) {
				HostLog("[oauth] Facebook end-live-video failed for " + live.id + ": " + err);
			} else if (resp.status < 200 || resp.status >= 300) {
				HostLog("[oauth] Facebook end-live-video for " + live.id + " returned HTTP " +
					std::to_string(resp.status));
			} else {
				DBG(LogCat::OAuth, "facebook live video %s ended", live.id.c_str());
			}
		}
	});
}

} // namespace OAuth
