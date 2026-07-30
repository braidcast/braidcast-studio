#include "facebook_provider.hpp"

#include <array>
#include <atomic>
#include <cstdint>
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
//
// business_management is not optional despite nothing here calling a business endpoint.
// Meta added it as a requirement of the /me/accounts edge itself in v17: a Page that is
// business-owned or on the New Pages Experience -- which is every Page that can be
// switched into like a profile, so in practice most of them -- is omitted from the list
// without it. The failure is silent, a 200 with an empty array, so the account connects
// and simply has nowhere to stream.
const std::array<const char *, 5> kFacebookScopes = {"pages_show_list", "pages_read_engagement", "pages_manage_posts",
						     "publish_video", "business_management"};

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

// The descriptor field that addresses one Page. The picker's key, the key
// enumerateTargets hands back so a caller can pre-set it, and the key ResolvePage reads
// off the pushed bag -- one name, so a rename cannot desync the three.
constexpr const char *kPageFieldKey = "page";

// The live-video node's concurrent-viewer field. Named once because the request asks for it
// explicitly and the response is read by the same name -- the two cannot desync, the way
// kPageFieldKey ties the picker's key to the key ResolvePage reads.
//
// Sourced from Meta's own generated Business SDK (facebook_business/adobjects/livevideo.py,
// LiveVideo.Field.live_views), not from the node reference: that page has answered 404 since
// early 2025. The SDK is codegen'd from the API's schema, so it is the closest thing to a
// primary source still reachable, but it carries no prose -- which is why a response missing
// this field is reported rather than assumed to mean zero.
constexpr const char *kLiveViewsField = "live_views";

// The statuses that mean a live video is no longer taking viewers. Written as a STOP list
// rather than an "is it live" allowlist on purpose: with the node reference gone, the exact
// spelling Meta returns for the LIVE state is unconfirmed, and an allowlist that guessed it
// wrong would silently suppress every real count. An unrecognized status therefore still
// reports, while these -- which only follow or cancel a broadcast -- do not.
//
// UNPUBLISHED is deliberately absent: it is the "Unpublished (Page admins only)" privacy
// choice this provider offers, which is a live broadcast with viewers, not an ended one.
const std::array<const char *, 5> kEndedLiveStatuses = {"VOD", "LIVE_STOPPED", "PROCESSING", "SCHEDULED_CANCELED",
							"SCHEDULED_EXPIRED"};

// A missing concurrent-viewer field is worth one loud line and no more: the poller re-reads
// every destination every 20 seconds, so warning per occurrence would fill the log for as long
// as the stream runs. Once per process, because a renamed field is a property of the API and
// not of any one destination.
std::atomic<bool> g_liveViewsMissingLogged{false};

using Http::AppendForm;
using JsonUtil::NumLoose;
using JsonUtil::Obj;
using JsonUtil::ParseJson;
using JsonUtil::Str;

bool IsEndedLiveStatus(const std::string &status)
{
	for (const char *ended : kEndedLiveStatuses) {
		if (status == ended) {
			return true;
		}
	}
	return false;
}

// A live video that answered without a usable viewer figure. Loud once on the host log so a
// renamed field surfaces without debug logging turned on, gated debug afterwards.
void ReportMissingLiveViews(const std::string &destTag, const std::string &videoId, const std::string &status)
{
	const std::string detail = "dest=" + destTag + " live video " + videoId + " (status " +
				   (status.empty() ? std::string("unreported") : status) + ")";
	if (!g_liveViewsMissingLogged.exchange(true)) {
		HostLog(std::string("[oauth] Facebook live-video read carried no `") + kLiveViewsField + "` for " +
			detail +
			"; this destination stays ABSENT from the viewer aggregate rather than counted as "
			"zero. If it persists on a broadcast that plainly has viewers, Meta has renamed the "
			"concurrent-viewer field.");
		return;
	}
	DBG(LogCat::OAuth, "facebook viewers: %s carried no %s -> leaving the count ABSENT", detail.c_str(),
	    kLiveViewsField);
}

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
	// content_tags takes a list even though the picker offers one choice, so a single
	// interest goes over as a one-element JSON array rather than a bare id.
	const std::string category = Str(Obj(fields, "category"), "id");
	if (!category.empty()) {
		AppendForm(body, "content_tags", json::array({category}).dump());
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
	// No Page field. The Page is WHICH DESTINATION this is, not what one broadcast says,
	// and every destination now claims exactly one -- so offering it per broadcast let a
	// user repoint a destination from the Go Live dialog, and let two destinations of one
	// account land on the same Page. It is chosen once, on the destination, and enforced
	// unique there (oauth.setTarget -> ClaimTargetForProfile).
	//
	// The claim still reaches applyMetadata: it lives in the remembered per-stream bag,
	// which the dialog restores wholesale and submits, so ResolvePage reads it without the
	// dialog having to render it.
	fields.push_back(json{{"key", "title"},
			      {"label", "Title"},
			      {"type", "text"},
			      {"tier", "simple"},
			      {"scope", "all"},
			      {"max", kMaxTitleLength}});
	fields.push_back(json{{"key", "description"},
			      {"label", "Description"},
			      {"type", "textarea"},
			      {"tier", "simple"},
			      {"scope", "all"}});
	// Required: Facebook rejects an empty status, and the value decides whether the
	// broadcast is visible to the Page's audience at all -- so the control must show the
	// value that will actually be sent rather than an unset dash.
	fields.push_back(json{{"key", "privacy"},
			      {"label", "Privacy"},
			      {"type", "enum"},
			      {"tier", "simple"},
			      {"scope", "channel"},
			      {"required", true},
			      {"default", kPrivacyOptions[0].value},
			      {"options", privacyOptions}});

	// content_tags, offered as the category. Meta has no category or game field for a live
	// video -- Facebook Gaming was sunset -- but content_tags is a real, searchable
	// vocabulary: /search?type=adinterest is its lookup, the same shape every other
	// provider's category picker already uses. Presenting it as "Category" describes what
	// it does for the user (says what the broadcast is about) without inventing a taxonomy.
	//
	// What a broadcast is about is the same on every channel of this account, but an
	// adinterest id is Meta's own -- no other platform's category picker can resolve one --
	// so the value it holds is shared no further than Facebook.
	fields.push_back(json{{"key", "category"},
			      {"label", "Category"},
			      {"type", "category"},
			      {"tier", "simple"},
			      {"scope", "provider"},
			      {"browsable", false},
			      {"placeholder", "Search interests\xE2\x80\xA6"}});
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

	// The account stays the person, deliberately, even though a destination is a Page.
	// Naming it after the Page held only while exactly one was reachable: at two Pages
	// the rule went silent and every destination sharing this account fell back to the
	// person's name and photo at once -- identical rows for different Pages, which is
	// the exact confusion the rename existed to prevent. A destination now carries its
	// own target identity (see StreamTarget::avatarUrl and the claimed-target bag in
	// target_destinations.cpp), so it is right at one Page and at five, and this layer
	// no longer has to guess which Page an account "is".
	return true;
}

bool FacebookProvider::FetchPages(OAuthAccount &acct, PageList &out, std::string &err)
{
	out.pages.clear();
	out.returned = 0;

	Http::HttpReq req;
	req.method = "GET";
	// 100 is Meta's per-page maximum for this edge and far beyond what a streamer
	// administers, so a single request answers without paging.
	req.url = GraphUrl("me/accounts?fields=id,name,access_token,picture.type(large)&limit=100");

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
	out.returned = data.size();
	for (const json &row : data) {
		PageRef page{Str(row, "id"), Str(row, "name"), Str(row, "access_token"),
			     Str(Obj(Obj(row, "picture"), "data"), "url")};
		// A Page whose token did not come back cannot be streamed to, and one with no id
		// cannot be addressed -- skip rather than offer a choice that would fail later.
		if (page.id.empty() || page.token.empty()) {
			continue;
		}
		if (page.name.empty()) {
			page.name = page.id;
		}
		out.pages.push_back(std::move(page));
	}
	// An empty list is the quietest failure this provider has: the connect succeeds, tokens
	// issue, and every downstream read simply finds nothing, so the account looks connected
	// while being unable to stream anywhere. Say so on the record, with the cause, because
	// nothing else in the flow reports it.
	if (out.returned == 0) {
		HostLog("[oauth] Facebook /me/accounts returned no Pages -- the grant covers none, so this "
			"account cannot stream. Check that the Meta app has a Login for Business "
			"configuration and that a Page was selected during consent.");
		return true;
	}
	// The dropped rows are otherwise invisible: every caller sees only the usable list, so
	// a grant that covers listing but not streaming would read as "no Pages" with nothing
	// on the record to say otherwise.
	if (out.pages.size() != out.returned) {
		HostLog("[oauth] Facebook /me/accounts listed " + std::to_string(out.returned) + " Page(s), " +
			std::to_string(out.pages.size()) + " usable; the rest returned no access token");
	}
	return true;
}

bool FacebookProvider::getMetadata(OAuthAccount &acct, json &out, std::string &err)
{
	(void)acct;
	(void)err;
	// Nothing to report, and deliberately no platform call. Create-per-go-live means there
	// is no live broadcast to read a title or description off, and the Page is no longer
	// seeded here: the reconcile claims it on the destination, so seeding it again would
	// put an unowned copy of the claim into the dialog's channel-wide bag -- the descriptor
	// no longer declares that key, so it would route there rather than to the stream.
	// Dropping it also takes a Pages request off every Go Live prefill.
	out = json::object();
	return true;
}

std::string FacebookProvider::targetFieldKey() const
{
	return kPageFieldKey;
}

bool FacebookProvider::enumerateTargets(OAuthAccount &acct, TargetList &out, std::string &err)
{
	out = TargetList{};

	PageList list;
	if (!FetchPages(acct, list, err)) {
		return false;
	}
	out.fieldKey = targetFieldKey();
	out.targets.reserve(list.pages.size());
	for (const PageRef &page : list.pages) {
		out.targets.push_back(StreamTarget{page.id, page.name, page.avatarUrl});
	}
	return true;
}

bool FacebookProvider::searchCategories(OAuthAccount &acct, const std::string &query, json &out, std::string &err)
{
	out = json::array();
	// No catalog to browse: adinterest is a search, and an empty query returns nothing
	// useful, so an unprompted open shows the placeholder rather than a fabricated list.
	// This is why the descriptor field is not `browsable`.
	if (query.empty()) {
		return true;
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url = GraphUrl("search?type=adinterest&limit=25&q=" + Http::UrlEncode(query));

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (!Http::Require2xx(resp, "Facebook interest search", err)) {
		return false;
	}

	const json j = ParseJson(resp.body);
	const json &data = Obj(j, "data");
	if (!data.is_array()) {
		err = "Facebook interest search response missing data";
		return false;
	}
	for (const json &row : data) {
		const std::string id = Str(row, "id");
		const std::string name = Str(row, "name");
		// A row missing either half cannot be offered: the id is what content_tags
		// carries and the name is the only thing the user can recognize it by.
		if (id.empty() || name.empty()) {
			continue;
		}
		out.push_back(json{{"id", id}, {"name", name}});
	}
	return true;
}

bool FacebookProvider::ResolvePage(OAuthAccount &acct, const json &fields, PageRef &out, std::string &err)
{
	const std::string chosenId = Str(Obj(fields, kPageFieldKey), "id");

	PageList list;
	if (!FetchPages(acct, list, err)) {
		return false;
	}
	if (list.pages.empty()) {
		// An empty usable list has two causes with two different remedies, and asserting
		// the first at a user who plainly does administer a Page sends them looking for a
		// problem that is not there.
		err = list.returned > 0
			      ? "Facebook listed this account's Pages but returned an access token for none of "
				"them; reconnect Facebook and grant access to the Page you stream to"
			      : "this Facebook account administers no Pages; Braidcast streams to Pages only";
		return false;
	}
	if (chosenId.empty()) {
		if (list.pages.size() > 1) {
			err = "choose which Facebook Page to stream to";
			return false;
		}
		out = list.pages.front();
		return true;
	}
	for (const PageRef &page : list.pages) {
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

bool FacebookProvider::viewerCounts(OAuthAccount &acct, std::map<DestinationId, int> &out, std::string &err)
{
	const std::string accountId = AccountId(acct);

	// Snapshot this account's live videos under the lock, then release it: every read below
	// blocks on Graph, and liveVideoMutex_ also guards the go-live commit and both stop hooks,
	// which run on the UI thread and must not wait on the network. The Page token is copied
	// out with the id because the request is Page-scoped -- the stored account holds the user
	// token, which cannot read a Page's live video.
	std::map<DestinationId, LiveVideo> targets;
	{
		const std::lock_guard<std::mutex> guard(liveVideoMutex_);
		for (const auto &entry : liveVideos_) {
			if (entry.first.accountId == accountId && !entry.second.id.empty()) {
				targets[entry.first] = entry.second;
			}
		}
	}
	// Nothing held means this account is not broadcasting through this app: a destination that
	// never went live, or one whose stop already popped its entry. False omits the account from
	// the aggregate, which is what the poller does with an unsupported or off-air platform.
	// Deliberately NOT re-discovered from /{page-id}/live_videos on an empty map: that would
	// spend a /me/accounts read plus one edge read per Page on every cycle of every idle
	// account, to recover only from a restart taken mid-broadcast.
	if (targets.empty()) {
		return false;
	}

	bool any = false;
	for (const auto &target : targets) {
		const std::string destTag = DestinationKey(target.first);

		Http::HttpReq req;
		req.method = "GET";
		// Both fields asked for EXPLICITLY. The node's default field set is the id and
		// little else, so an unqualified read answers 200 with nothing to count.
		req.url = GraphUrl(target.second.id) + "?fields=" + kLiveViewsField + ",status";

		OAuthAccount pageAcct = PageAccount(id(), target.second.pageToken);
		Http::HttpResponse resp;
		std::string readErr;
		// One destination failing must not discard the ones that read: keep the first error
		// for the caller's log and carry on, so a partial total still beats no total.
		if (!SendAuthed(pageAcct, req, resp, readErr) ||
		    !Http::Require2xx(resp, "Facebook live-video viewers request", readErr)) {
			if (err.empty()) {
				err = readErr;
			}
			continue;
		}

		const json j = ParseJson(resp.body);
		const std::string status = Str(j, "status");
		// The stop hooks pop an entry the moment an output ends, so this covers only what
		// they cannot see: a broadcast ended on Meta's side -- cut off, or ended from the
		// Page -- while the local output keeps running. Its last figure would otherwise be
		// re-reported every cycle as though the audience were still there.
		if (IsEndedLiveStatus(status)) {
			DBG(LogCat::OAuth, "facebook viewers: dest=%s live video %s is %s -> leaving the count ABSENT",
			    destTag.c_str(), target.second.id.c_str(), status.c_str());
			continue;
		}

		// -1 as the sentinel separates "missing, null, or not a number" from a genuine 0
		// without a second lookup: a concurrent-viewer count is never negative.
		const int64_t views = NumLoose(j, kLiveViewsField, -1);
		if (views < 0) {
			ReportMissingLiveViews(destTag, target.second.id, status);
			continue;
		}
		out[target.first] = static_cast<int>(views);
		any = true;
		DBG(LogCat::OAuth, "facebook viewers: dest=%s live video %s -> %d concurrent viewers", destTag.c_str(),
		    target.second.id.c_str(), static_cast<int>(views));
	}
	// `err` is deliberately LEFT SET on a partial read: returning true says "these rows are
	// usable", and the caller reports a non-empty err alongside them so a dropped destination
	// is still visible in the log.
	return any;
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
