#include "youtube_provider.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

#include "../chat/youtube_chat.hpp"
#include "account_store.hpp"
#include "util/http_client.hpp"
#include "../ingest_writeback.hpp"
#include "util/json_util.hpp"
#include "util/op_error.hpp"
#include "../log.hpp"
#include "ui-config.h"

namespace OAuth {

namespace {

// YouTube Data API v3 endpoints. The live lifecycle creates a fresh broadcast per
// Go Live (create-per-go-live), binds it to the account's reusable ingest stream
// (created once, remembered per account, re-verified each go-live), then tags the
// resulting video. Verified against the YouTube Data API v3 reference (2026-06).
// Note: create-per-go-live can leave an orphaned broadcast in the user's account
// if a CRITICAL step fails after the insert -- inherent to the model, not a leak
// (no live resource is held; the stale entity is just unused).
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

// Throttles the liveBroadcasts.list cache-miss probe, per ACCOUNT: one probe attributes
// every destination of that account, so several destinations missing the cache together
// still cost one call, and an off-air account cannot burn API quota every poll cycle.
constexpr std::chrono::seconds kBroadcastProbeThrottle{15};

// error.errors[0].reason values on a 403/429 when the app is briefly rate-limited:
// transient within seconds, worth a backoff retry.
constexpr const char *kRateLimitReasons[] = {
	"rateLimitExceeded",
	"userRateLimitExceeded",
};

// error.errors[0].reason values on a 403 when the project's DAILY quota is spent:
// lasts until the next midnight-Pacific reset, so any earlier retry is wasted traffic.
constexpr const char *kQuotaExhaustedReasons[] = {
	"quotaExceeded",
	"dailyLimitExceeded",
};

// Slack past the computed midnight before requests resume, so a wake on a
// slightly-fast local clock cannot re-observe quotaExceeded and re-arm the gate
// for a full extra day.
constexpr int64_t kQuotaResetSlackSec = 300;

// Furthest ahead of now a RESTORED reset instant can plausibly sit. A real one is the next
// midnight Pacific computed at some moment in the past, so it is at most ~24h out; 26h
// covers a DST shift plus the slack above. Anything beyond is corrupt or clock-skewed and is
// discarded rather than honored -- see EnsureQuotaStateLoaded for why failing open is right.
constexpr int64_t kQuotaResetHorizonSec = 26 * 60 * 60;

// Ids per videos.list request. The endpoint takes a comma-separated id list capped at 50 (per
// the Data API reference) and bills one unit per REQUEST regardless, so batching up to this
// many broadcasts costs what a single one used to. At ~11 characters per id the longest URL
// this can build is a few hundred bytes, far inside any practical limit.
constexpr size_t kVideosListIdCap = 50;

using JsonUtil::Bool;
using JsonUtil::Obj;
using JsonUtil::ParseJson;
using JsonUtil::Str;

template<size_t N> bool ReasonIn(const std::string &reason, const char *const (&list)[N])
{
	return std::any_of(std::begin(list), std::end(list), [&](const char *r) { return reason == r; });
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's days-from-civil).
int64_t DaysFromCivil(int y, int m, int d)
{
	y -= m <= 2;
	const int era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(y - era * 400);
	const unsigned mp = static_cast<unsigned>(m + (m > 2 ? -3 : 9));
	const unsigned doy = (153u * mp + 2) / 5 + static_cast<unsigned>(d) - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

// The civil year containing epoch day `z` (the year component of civil-from-days).
int YearFromDays(int64_t z)
{
	z += 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const unsigned doe = static_cast<unsigned>(z - era * 146097);
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
	const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned mp = (5 * doy + 2) / 153;
	const unsigned m = mp < 10 ? mp + 3 : mp - 9;
	return y + (m <= 2);
}

// Epoch seconds of the Nth Sunday of `month` in `year`, at `utcHour`:00 UTC.
int64_t NthSundayUtc(int year, int month, int nth, int utcHour)
{
	const int64_t first = DaysFromCivil(year, month, 1);
	const int dow = static_cast<int>((first + 4) % 7); // 1970-01-01 was a Thursday; 0 = Sunday
	const int64_t day = first + (7 - dow) % 7 + 7 * (nth - 1);
	return day * 86400 + utcHour * 3600;
}

// Pacific Time's UTC offset at `utcEpoch`, honoring US DST (starts the second
// Sunday of March at 02:00 standard = 10:00 UTC, ends the first Sunday of November
// at 02:00 daylight = 09:00 UTC): UTC-7 inside the window, UTC-8 outside.
int64_t PacificOffsetSec(int64_t utcEpoch)
{
	const int year = YearFromDays(utcEpoch / 86400);
	const int64_t dstStart = NthSundayUtc(year, 3, 2, 10);
	const int64_t dstEnd = NthSundayUtc(year, 11, 1, 9);
	return (utcEpoch >= dstStart && utcEpoch < dstEnd) ? -7 * 3600 : -8 * 3600;
}

// Epoch seconds of the next midnight Pacific after `nowUtc` -- the instant the
// YouTube Data API's daily quota resets.
int64_t NextPacificMidnightUtc(int64_t nowUtc)
{
	const int64_t offsetNow = PacificOffsetSec(nowUtc);
	const int64_t nextLocalMidnight = ((nowUtc + offsetNow) / 86400 + 1) * 86400;
	// Convert back with the offset in force AT the reset, so a DST flip between now
	// and midnight cannot shift the instant by an hour.
	return nextLocalMidnight - PacificOffsetSec(nextLocalMidnight - offsetNow);
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
	const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
		return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
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

std::string YouTubeErrorReason(const std::string &body)
{
	const json errJson = ParseJson(body);
	const json &errors = Obj(Obj(errJson, "error"), "errors");
	if (errors.is_array() && !errors.empty()) {
		return Str(errors[0], "reason");
	}
	return std::string();
}

YouTubeErrorClass ClassifyYouTubeError(long status, const std::string &reason)
{
	if (ReasonIn(reason, kQuotaExhaustedReasons)) {
		return YouTubeErrorClass::QuotaExhausted;
	}
	if (status == 429 || ReasonIn(reason, kRateLimitReasons)) {
		return YouTubeErrorClass::RateLimited;
	}
	return YouTubeErrorClass::Other;
}

void YouTubeProvider::EnsureQuotaStateLoaded() const
{
	std::call_once(quotaLoadOnce_, [this] {
		// MAXIMUM across this provider's accounts, not the first found: the verdict is
		// provider-wide and written to every account, so the newest surviving record is the
		// authoritative one even if another was disconnected or written before a crash.
		int64_t stored = 0;
		for (const auto &entry : Accounts().All()) {
			if (entry.second.providerId == id() && entry.second.quotaResetEpoch > stored) {
				stored = entry.second.quotaResetEpoch;
			}
		}
		if (stored <= 0) {
			return;
		}
		// FAIL OPEN on anything that cannot be a real reset instant. A legitimate value was
		// computed as "the next midnight Pacific" at some past moment, so it can never sit
		// more than a day (plus slack) ahead of now; further out means a corrupt, hand-edited,
		// or clock-skewed record, and honoring it would silence every YouTube feature for as
		// long as it says. One wasted request is the cheaper error.
		const int64_t now = static_cast<int64_t>(std::time(nullptr));
		if (stored > now + kQuotaResetHorizonSec) {
			HostLog("[oauth] YouTube stored quota reset is implausibly far ahead; ignoring it");
			return;
		}
		if (stored <= now) {
			return; // already elapsed: nothing to restore
		}
		quotaResetEpoch_.store(stored, std::memory_order_release);
		HostLog("[oauth] YouTube API daily quota still exhausted from a previous session; "
			"suspending YouTube requests until " +
			QuotaResetLocalTime() + " local (midnight Pacific)");
	});
}

void YouTubeProvider::NoteQuotaExhausted(const std::string &reason)
{
	EnsureQuotaStateLoaded();
	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	int64_t current = quotaResetEpoch_.load(std::memory_order_acquire);
	if (current > now) {
		return; // this episode is already recorded (and logged)
	}
	const int64_t reset = NextPacificMidnightUtc(now) + kQuotaResetSlackSec;
	if (!quotaResetEpoch_.compare_exchange_strong(current, reset, std::memory_order_acq_rel)) {
		return; // another worker recorded this episode first
	}
	// Persist to EVERY account of this provider, so the next launch stands down without
	// re-learning the verdict by spending a request that is guaranteed to fail. Field-scoped
	// per account (SetQuotaReset) rather than a record write-back: this runs on whichever
	// worker received the 403.
	for (const auto &entry : Accounts().All()) {
		if (entry.second.providerId == id()) {
			Accounts().SetQuotaReset(entry.first, reset);
		}
	}
	HostLog("[oauth] YouTube API daily quota exhausted (" + reason + "); suspending YouTube requests until " +
		QuotaResetLocalTime() + " local (midnight Pacific)");
}

bool YouTubeProvider::QuotaExhausted(std::chrono::milliseconds *retryIn) const
{
	EnsureQuotaStateLoaded();
	const int64_t reset = quotaResetEpoch_.load(std::memory_order_acquire);
	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	if (reset <= now) {
		return false;
	}
	if (retryIn) {
		*retryIn = std::chrono::milliseconds((reset - now) * 1000);
	}
	return true;
}

std::string YouTubeProvider::QuotaResetLocalTime() const
{
	const int64_t reset = quotaResetEpoch_.load(std::memory_order_acquire);
	if (reset == 0) {
		return std::string();
	}
	const std::time_t t = static_cast<std::time_t>(reset);
	std::tm tm{};
	localtime_s(&tm, &t);
	char buf[8];
	std::strftime(buf, sizeof buf, "%H:%M", &tm);
	return std::string(buf);
}

std::string YouTubeProvider::QuotaMessage() const
{
	return "YouTube API quota exhausted; retries resume after " + QuotaResetLocalTime();
}

void YouTubeProvider::NoteIfQuotaError(long status, const std::string &body)
{
	if (status != 403) {
		return;
	}
	const std::string reason = YouTubeErrorReason(body);
	if (ClassifyYouTubeError(status, reason) == YouTubeErrorClass::QuotaExhausted) {
		NoteQuotaExhausted(reason);
	}
}

bool YouTubeProvider::SendAuthed(OAuthAccount &acct, Http::HttpReq req, Http::HttpResponse &resp, std::string &err)
{
	if (QuotaExhausted()) {
		// The one YouTube refusal with a streamer-ready sentence: pack it as the
		// user message too, so the UI can show it bare while every wrapper up the
		// stack keeps prefixing the diagnostic for the logs.
		const std::string msg = QuotaMessage();
		err = Err::User(msg, msg);
		return false;
	}
	if (!StreamProvider::SendAuthed(acct, std::move(req), resp, err)) {
		return false;
	}
	NoteIfQuotaError(resp.status, resp.body);
	return true;
}

long YouTubeProvider::SendAuthedStreaming(OAuthAccount &acct, Http::HttpReq req,
					  const std::function<bool(std::string_view chunk)> &onChunk,
					  std::string &errorBody, std::string &err)
{
	if (QuotaExhausted()) {
		errorBody.clear();
		const std::string msg = QuotaMessage();
		err = Err::User(msg, msg);
		return 0;
	}
	const long status = StreamProvider::SendAuthedStreaming(acct, std::move(req), onChunk, errorBody, err);
	NoteIfQuotaError(status, errorBody);
	return status;
}

YouTubeProvider::YouTubeProvider()
	: auth_(BrokerStrategy::Config{
		  BRAIDCAST_BROKER_URL,  // brokerBaseUrl
		  "youtube",             // platform
		  YOUTUBE_SCOPE_VERSION, // scopeVer
		  true,                  // revokePreferAccessToken -- Google's docs confirm this also revokes the paired refresh token
	  })
{
}

// Out-of-line default dtor: the header only forward-declares Chat::YouTubeChat (used
// by makeChat + the friend grant), so the translation unit that destroys the provider
// needs the complete-type context this .cpp provides.
YouTubeProvider::~YouTubeProvider() = default;

std::unique_ptr<Chat::ChatTransport> YouTubeProvider::makeChat(const OAuthAccount &acct)
{
	(void)acct; // the hub resolves the destination's liveChatId and passes it into connect()
	return std::make_unique<Chat::YouTubeChat>(*this);
}

std::unique_ptr<Events::EventTransport> YouTubeProvider::makeEvents(const OAuthAccount &acct)
{
	(void)acct; // YouTubeEvents reads acct fresh per call via SendAuthed
	return std::make_unique<Events::YouTubeEvents>(this);
}

std::string YouTubeProvider::chatChannelRef(const OAuthAccount &acct, const std::string &profileUuid)
{
	const std::lock_guard<std::mutex> guard(broadcastMutex_);
	auto it = broadcasts_.find(DestinationId{AccountId(acct), profileUuid});
	return it != broadcasts_.end() ? it->second.liveChatId : std::string();
}

void YouTubeProvider::clearActiveBroadcast(const std::string &accountId)
{
	const std::lock_guard<std::mutex> guard(broadcastMutex_);
	// Every destination of this account, not one entry: the account may hold several
	// concurrent broadcasts and a stop ends all of them.
	for (auto it = broadcasts_.begin(); it != broadcasts_.end();) {
		it = it->first.accountId == accountId ? broadcasts_.erase(it) : std::next(it);
	}
}

void YouTubeProvider::clearActiveBroadcastDestination(const DestinationId &dest)
{
	const std::lock_guard<std::mutex> guard(broadcastMutex_);
	broadcasts_.erase(dest);
}

void YouTubeProvider::AddLiveChatRef(const DestinationId &dest)
{
	const std::lock_guard<std::mutex> guard(liveChatMutex_);
	++liveChatRefs_[dest];
}

void YouTubeProvider::ReleaseLiveChatRef(const DestinationId &dest)
{
	const std::lock_guard<std::mutex> guard(liveChatMutex_);
	auto it = liveChatRefs_.find(dest);
	if (it == liveChatRefs_.end()) {
		return;
	}
	if (--it->second <= 0) {
		liveChatRefs_.erase(it);
	}
}

bool YouTubeProvider::ShouldPollSuperChats(const std::string &accountId) const
{
	// Snapshot the account's live destinations, then release broadcastMutex_ before taking
	// liveChatMutex_: the two are never held together, so no lock order exists to invert.
	std::vector<DestinationId> liveDestinations;
	{
		const std::lock_guard<std::mutex> guard(broadcastMutex_);
		for (const auto &entry : broadcasts_) {
			if (entry.first.accountId == accountId && !entry.second.broadcastId.empty()) {
				liveDestinations.push_back(entry.first);
			}
		}
	}
	if (liveDestinations.empty()) {
		return false; // not broadcasting: nothing this read could return
	}

	const std::lock_guard<std::mutex> guard(liveChatMutex_);
	for (const DestinationId &dest : liveDestinations) {
		auto it = liveChatRefs_.find(dest);
		if (it == liveChatRefs_.end() || it->second <= 0) {
			return true; // this broadcast's Super Chats have no other path in
		}
	}
	return false;
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
			      {"max", 500}});
	fields.push_back(json{{"key", "thumbnail"},
			      {"label", "Thumbnail"},
			      {"type", "image"},
			      {"tier", "simple"},
			      {"shareable", false}});
	fields.push_back(json{{"key", "description"},
			      {"label", "Description"},
			      {"type", "textarea"},
			      {"tier", "simple"},
			      {"shareable", true}});
	// Privacy defaults to "private": broadcasting publicly must be an explicit
	// choice, never the result of leaving the field untouched. The modal's prefill
	// seeds this default only when no remembered value exists for the account.
	fields.push_back(json{{"key", "privacy"},
			      {"label", "Privacy"},
			      {"type", "enum"},
			      {"tier", "simple"},
			      {"shareable", false},
			      {"default", "private"},
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
	fields.push_back(
		json{{"key", "dvr"}, {"label", "DVR"}, {"type", "bool"}, {"tier", "advanced"}, {"shareable", false}});
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
		{"auth", json{{"strategy", "broker"}, {"scopes", scopes}, {"needsSecret", false}}},
		{"fields", fields},
	};
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
	if (snippet.is_object() && snippet.contains("thumbnails") && snippet["thumbnails"].is_object()) {
		const json &thumbs = snippet["thumbnails"];
		for (const char *size : {"high", "medium", "default"}) {
			if (thumbs.contains(size) && thumbs[size].is_object()) {
				acct.avatarUrl = Str(thumbs[size], "url");
				if (!acct.avatarUrl.empty()) {
					break;
				}
			}
		}
	}
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
					if (!row.is_object() || !row.contains("snippet") ||
					    !row["snippet"].is_object()) {
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
				    bool goingLive, std::string &err)
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
		// Last-resort guard only -- the real default is the capability field's
		// remembered-else-"private" (seeded by the modal). An absent/invalid value
		// must resolve to the safest visibility; it must never silently go public.
		privacy = "private";
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
	// failed (re-)apply doesn't leave an orphan liveBroadcast on the channel. Deliberately
	// deletes ONLY the broadcast, never the liveStream: the stream is the account's
	// reusable ingest resource (see step 2) that every future go-live re-binds. Errors here
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
			HostLog("[oauth] YouTube orphan liveBroadcasts.delete failed (ignored): " +
				Err::Diagnostic(delErr));
		} else if (resp.status < 200 || resp.status >= 300) {
			HostLog("[oauth] YouTube orphan liveBroadcasts.delete failed (ignored): HTTP " +
				std::to_string(resp.status) + ": " + resp.body);
		}
	};

	// Steps 4 + 5 (video category/tags, then thumbnail) apply identically whether the
	// broadcast was just created for go-live or is already live for a mid-stream edit, so
	// both paths call this instead of duplicating the blocks. Both are NON-CRITICAL:
	// failures are logged and skipped, never surfaced as an apply failure.
	auto applyVideoTagsAndThumbnail = [&](const std::string &videoId) {
		// 4. videos.update -- category + tags live on the video, not the broadcast.
		// part=snippet REPLACES the whole snippet, so title + categoryId must be
		// re-sent or the call 400s / wipes them. Only worth a call when a category was
		// chosen or tags exist; categoryId 24 (Entertainment) is a safe assignable
		// default needed only when tags exist without a chosen category.
		if (!categoryId.empty() || !tags.empty()) {
			const std::string effectiveCategory = categoryId.empty() ? "24" : categoryId;
			json videoSnippet = json{
				{"title", title},
				{"description", description},
				{"categoryId", effectiveCategory},
				{"tags", tags},
			};
			json videoBody = json{{"id", videoId}, {"snippet", videoSnippet}};
			json vResp;
			std::string vErr;
			if (!sendJson("PUT", std::string(kVideosUrl) + "?part=snippet", videoBody, vResp, vErr)) {
				HostLog("[oauth] YouTube videos.update failed (continuing): " + Err::Diagnostic(vErr));
			}
		}

		// 5. thumbnails.set -- raw binary upload (image bytes as the body, sniffed
		// Content-Type), NOT multipart. data: URLs are Phase 8e.
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
					req.url =
						std::string(kThumbnailsSetUrl) + "?videoId=" + Http::UrlEncode(videoId);
					req.contentType = SniffImageMime(bytes);
					req.body = bytes;
					Http::HttpResponse resp;
					std::string thumbErr;
					if (!SendAuthed(acct, req, resp, thumbErr)) {
						HostLog("[oauth] YouTube thumbnails.set failed (continuing): " +
							Err::Diagnostic(thumbErr));
					} else if (resp.status < 200 || resp.status >= 300) {
						HostLog("[oauth] YouTube thumbnails.set failed (continuing): HTTP " +
							std::to_string(resp.status) + ": " + resp.body);
					}
				}
			}
		} else if (!thumbnailPath.empty()) {
			HostLog("[oauth] YouTube thumbnail skipped: data: URL handling is Phase 8e");
		}
	};

	// A standalone "Edit stream info" push (not the prelude to streaming.start). Under
	// create-per-go-live a broadcast exists ONLY during/after go-live, so this edit must
	// never insert/bind a broadcast: doing so would leave a stale "Upcoming" broadcast
	// pre-live, or rebind ingest and break a running stream when edited mid-stream.
	if (!goingLive) {
		BroadcastState active;
		if (EnsureActiveBroadcast(acct, profileUuid, active, err)) {
			// Mid-stream edit: update the live broadcast in place. No insert, no bind, no
			// ingest rebind, no broadcasts_ mutation -- only its editable metadata. YouTube
			// requires snippet.title AND snippet.scheduledStartTime together on a
			// part=snippet update and rejects a fresh time for an already-started
			// broadcast, so echo the existing scheduledStartTime read back here.
			Http::HttpReq getReq;
			getReq.method = "GET";
			getReq.url = std::string(kLiveBroadcastsUrl) +
				     "?part=snippet,status&id=" + Http::UrlEncode(active.broadcastId);
			Http::HttpResponse getResp;
			if (!SendAuthed(acct, getReq, getResp, err)) {
				return false;
			}
			if (getResp.status < 200 || getResp.status >= 300) {
				err = "YouTube liveBroadcasts.list failed (HTTP " + std::to_string(getResp.status) +
				      "): " + getResp.body;
				return false;
			}
			const json existing = FirstItem(ParseJson(getResp.body));
			std::string scheduledStart;
			if (existing.is_object() && existing.contains("snippet") && existing["snippet"].is_object()) {
				scheduledStart = Str(existing["snippet"], "scheduledStartTime");
			}
			if (scheduledStart.empty()) {
				scheduledStart = NowIso8601Utc();
			}

			// contentDetails (latency/dvr/autoStart) is intentionally omitted: those are
			// go-live-time settings, immutable once the broadcast is testing/live (a PUT
			// including them would 400).
			json updSnippet = json{{"title", title},
					       {"description", description},
					       {"scheduledStartTime", scheduledStart}};
			json updStatus = json{{"privacyStatus", privacy}, {"selfDeclaredMadeForKids", madeForKids}};
			json updBody = json{{"id", active.broadcastId}, {"snippet", updSnippet}, {"status", updStatus}};
			json updResp;
			std::string updErr;
			if (!sendJson("PUT", std::string(kLiveBroadcastsUrl) + "?part=snippet,status", updBody, updResp,
				      updErr)) {
				err = Err::Wrap("YouTube liveBroadcasts.update failed: ", updErr);
				return false;
			}

			applyVideoTagsAndThumbnail(active.broadcastId);
			return true;
		}
		// EnsureActiveBroadcast returned false: a non-empty err is a genuine API/network
		// failure (propagate it); an EMPTY err means no active broadcast (or the probe was
		// throttled) -- a pre-live edit with no target to update. The metadata is already
		// remembered client-side (streamMeta.save) and gets applied when Go Live creates
		// the broadcast, so no-op here rather than creating one.
		if (!err.empty()) {
			return false;
		}
		return true;
	}

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
		err = Err::Wrap("YouTube liveBroadcasts.insert failed: ", stepErr);
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

	// 2. Resolve the RTMP ingest endpoint + stream key. CRITICAL. The liveStream is a
	// reusable per-account resource: liveStreams.insert costs 50 quota units, so the
	// remembered stream is re-verified (liveStreams.list, 1 unit) and re-bound each
	// go-live, and a fresh one is inserted only when no remembered stream survives.
	// The ingest address + key are always read from the live API response, never from
	// a cache -- YouTube may rotate the key server-side between go-lives.
	std::string streamId;
	std::string ingestionAddress;
	std::string streamName;

	// Pulls id + cdn.ingestionInfo out of a liveStreams resource; false when any of
	// the three fields the RTMP output needs is missing.
	auto readIngestion = [&](const json &item) -> bool {
		streamId = Str(item, "id");
		ingestionAddress.clear();
		streamName.clear();
		if (item.is_object() && item.contains("cdn") && item["cdn"].is_object()) {
			const json &cdn = item["cdn"];
			if (cdn.contains("ingestionInfo") && cdn["ingestionInfo"].is_object()) {
				ingestionAddress = Str(cdn["ingestionInfo"], "ingestionAddress");
				streamName = Str(cdn["ingestionInfo"], "streamName");
			}
		}
		return !streamId.empty() && !streamName.empty() && !ingestionAddress.empty();
	};

	// THIS DESTINATION's remembered stream, never the account's: a liveStream carries one
	// video feed and binds to one broadcast at a time, so two profiles on one channel must
	// hold two separate ingest endpoints or they would share an RTMP key and the second
	// go-live's bind would detach the first broadcast.
	std::string rememberedStreamId;
	if (const std::optional<OAuthAccount> stored = Accounts().Get(AccountId(acct))) {
		const auto it = stored->reusableStreamIds.find(profileUuid);
		if (it != stored->reusableStreamIds.end()) {
			rememberedStreamId = it->second;
		}
	}
	if (!rememberedStreamId.empty()) {
		// Reuse only a stream the API just confirmed exists and is not errored. ANY
		// other outcome (deleted server-side, missing ingest info, transport failure)
		// falls through to a fresh insert -- a stale cached id must never be the
		// reason a go-live fails. The id filter alone is correct: liveStreams.list
		// forbids combining id with mine, and the bearer already scopes the lookup
		// to this account's own streams (a foreign/deleted id returns empty items).
		Http::HttpReq verifyReq;
		verifyReq.method = "GET";
		verifyReq.url = std::string(kLiveStreamsUrl) +
				"?part=id,cdn,status&id=" + Http::UrlEncode(rememberedStreamId);
		Http::HttpResponse verifyResp;
		std::string verifyErr;
		if (SendAuthed(acct, verifyReq, verifyResp, verifyErr) && verifyResp.status >= 200 &&
		    verifyResp.status < 300) {
			const json item = FirstItem(ParseJson(verifyResp.body));
			const bool errored = item.is_object() && item.contains("status") &&
					     item["status"].is_object() &&
					     Str(item["status"], "streamStatus") == "error";
			if (!errored && readIngestion(item) && streamId == rememberedStreamId) {
				HostLog("[oauth] YouTube reusing ingest stream " + streamId);
			} else {
				streamId.clear();
			}
		}
		if (streamId.empty()) {
			HostLog("[oauth] YouTube remembered ingest stream " + rememberedStreamId +
				" not verified; creating a fresh one");
		}
	}

	if (streamId.empty()) {
		// A fixed name, not the broadcast title: this resource outlives the broadcast
		// that happened to create it and shows up in the channel's stream list, where
		// one go-live's title would be a confusing label for every later one.
		json streamBody = json{
			{"snippet", json{{"title", "Braidcast ingest"}}},
			{"cdn", json{{"frameRate", "variable"}, {"ingestionType", "rtmp"}, {"resolution", "variable"}}},
			{"contentDetails", json{{"isReusable", true}}},
		};
		json sResp;
		if (!sendJson("POST", std::string(kLiveStreamsUrl) + "?part=snippet,cdn,contentDetails", streamBody,
			      sResp, stepErr)) {
			err = Err::Wrap("YouTube liveStreams.insert failed: ", stepErr);
			deleteOrphanBroadcast(broadcastId);
			return false;
		}
		if (!readIngestion(sResp)) {
			err = "YouTube liveStreams.insert returned no stream key";
			deleteOrphanBroadcast(broadcastId);
			return false;
		}
		// Remember the stream the moment it exists: even if a later go-live step
		// fails, the resource is already on the channel, and the next go-live should
		// find it via the 1-unit verify instead of paying for another insert. Recorded
		// against this destination, so a sibling profile keeps its own stream.
		Accounts().UpdateReusableStreamId(AccountId(acct), profileUuid, streamId);
	}

	// 3. liveBroadcasts.bind -- attach the stream to the broadcast. CRITICAL.
	const std::string bindUrl = std::string(kLiveBroadcastsUrl) + "/bind?id=" + Http::UrlEncode(broadcastId) +
				    "&streamId=" + Http::UrlEncode(streamId) + "&part=id,contentDetails";
	json bindResp;
	if (!sendJson("POST", bindUrl, json::object(), bindResp, stepErr)) {
		err = Err::Wrap("YouTube liveBroadcasts.bind failed: ", stepErr);
		deleteOrphanBroadcast(broadcastId);
		return false;
	}

	// The new broadcast now exists and is bound. Only here do we invalidate THIS
	// DESTINATION's prior broadcast chat + viewer-count target -- if any earlier step had
	// failed, the previously-live broadcast stays intact rather than being torn down for
	// one that never came up. Scoped to this destination so a second profile going live on
	// the same account leaves the first profile's live broadcast alone. The new ids are
	// committed once this apply fully succeeds (below).
	const DestinationId dest{AccountId(acct), profileUuid};
	{
		const std::lock_guard<std::mutex> guard(broadcastMutex_);
		BroadcastState &bs = broadcasts_[dest];
		bs.liveChatId.clear();
		bs.broadcastId.clear();
	}

	// 4 + 5. Video category/tags, then thumbnail (both NON-CRITICAL). Shared with the
	// mid-stream edit path.
	applyVideoTagsAndThumbnail(broadcastId);

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
		BroadcastState &bs = broadcasts_[dest];
		bs.liveChatId = liveChatId;
		bs.broadcastId = broadcastId;
	}
	return true;
}

bool YouTubeProvider::ProbeActiveBroadcasts(OAuthAccount &acct, std::string &err)
{
	err.clear();
	const std::string accountId = AccountId(acct);

	{
		const std::lock_guard<std::mutex> guard(broadcastMutex_);
		const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		auto probeIt = lastBroadcastProbe_.find(accountId);
		if (probeIt != lastBroadcastProbe_.end() && now - probeIt->second < kBroadcastProbeThrottle) {
			return false;
		}
		lastBroadcastProbe_[accountId] = now;
	}

	// Which stream backs which destination. Read before taking broadcastMutex_ (the account
	// store has its own lock, and nesting the two would invite a lock-order inversion) and
	// read from the store rather than the caller's copy so a stream inserted by a concurrent
	// go-live is visible. Without any remembered stream there is nothing to match against.
	std::map<std::string /*streamId*/, std::string /*profileUuid*/> profileByStream;
	if (const std::optional<OAuthAccount> stored = Accounts().Get(accountId)) {
		for (const auto &entry : stored->reusableStreamIds) {
			if (!entry.second.empty()) {
				profileByStream[entry.second] = entry.first;
			}
		}
	}

	// contentDetails carries boundStreamId, and liveBroadcasts.list bills the same 1 unit
	// whichever parts are requested -- so the attribution below is free.
	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kLiveBroadcastsUrl) +
		  "?part=id,snippet,contentDetails&broadcastStatus=active&mine=true&maxResults=50";

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "YouTube liveBroadcasts request failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	const json parsed = ParseJson(resp.body);
	if (!parsed.is_object() || !parsed.contains("items") || !parsed["items"].is_array()) {
		return false; // no active broadcast -> nothing live for this account
	}

	// Attribute each active broadcast to the destination whose remembered ingest stream it
	// is bound to. This is EXACT, not a guess: a stream belongs to exactly one destination
	// and a broadcast is bound to exactly one stream, so the mapping is unambiguous even
	// with several orientations live -- and it survives a restart because the stream ids are
	// persisted. A broadcast bound to a stream we do not recognize (created outside this
	// app, or one whose id was discarded on upgrade) is left alone rather than misattributed.
	std::map<DestinationId, BroadcastState> learned;
	size_t unattributed = 0;
	for (const json &item : parsed["items"]) {
		if (!item.is_object()) {
			continue;
		}
		BroadcastState bs;
		bs.broadcastId = Str(item, "id");
		if (bs.broadcastId.empty()) {
			continue;
		}
		if (item.contains("snippet") && item["snippet"].is_object()) {
			// Recovering liveChatId here (not just broadcastId) means the next
			// chatChannelRef read picks it up for free -- no separate network call.
			bs.liveChatId = Str(item["snippet"], "liveChatId");
		}
		std::string boundStreamId;
		if (item.contains("contentDetails") && item["contentDetails"].is_object()) {
			boundStreamId = Str(item["contentDetails"], "boundStreamId");
		}
		const auto owner = profileByStream.find(boundStreamId);
		if (boundStreamId.empty() || owner == profileByStream.end()) {
			++unattributed;
			continue;
		}
		learned[DestinationId{accountId, owner->second}] = bs;
	}

	if (unattributed > 0) {
		HostLog("[oauth] YouTube skipped " + std::to_string(unattributed) +
			" active broadcast(s) for " + accountId + " bound to an unrecognized ingest stream");
	}
	if (learned.empty()) {
		return false;
	}

	{
		const std::lock_guard<std::mutex> guard(broadcastMutex_);
		for (const auto &entry : learned) {
			// Fill GAPS ONLY, never overwrite. applyMetadata is authoritative -- it knows
			// exactly which broadcast it created for which destination -- whereas this is
			// an HTTP snapshot that can already be stale by the time the lock is taken: a
			// go-live may have committed while the probe was in flight, and YouTube can
			// still list a just-ended broadcast against the same ingest stream. Replacing a
			// populated entry would swap a live liveChatId for a dying broadcast's, and the
			// cache-hit path would never re-probe to notice.
			BroadcastState &slot = broadcasts_[entry.first];
			if (slot.broadcastId.empty()) {
				slot = entry.second;
			}
		}
	}
	return true;
}

bool YouTubeProvider::EnsureActiveBroadcast(OAuthAccount &acct, const std::string &profileUuid, BroadcastState &out,
					   std::string &err)
{
	err.clear();
	const DestinationId dest{AccountId(acct), profileUuid};

	// Cache first; the probe is only for a miss (a restart mid-stream, or a broadcast
	// started outside this session). broadcastMutex_ is never held across the probe's HTTP
	// call -- it would stall chatChannelRef/clearActiveBroadcast on other threads.
	{
		const std::lock_guard<std::mutex> guard(broadcastMutex_);
		auto it = broadcasts_.find(dest);
		if (it != broadcasts_.end() && !it->second.broadcastId.empty()) {
			out = it->second;
			return true;
		}
	}

	if (!ProbeActiveBroadcasts(acct, err)) {
		return false;
	}

	const std::lock_guard<std::mutex> guard(broadcastMutex_);
	auto it = broadcasts_.find(dest);
	if (it == broadcasts_.end() || it->second.broadcastId.empty()) {
		return false; // the account is live, but not on this destination
	}
	out = it->second;
	return true;
}

// Read concurrent viewers for a BATCH of broadcasts (videos.list liveStreamingDetails).
// videos.list accepts a comma-separated id list and bills per REQUEST, not per id, so an
// account streaming several orientations costs one unit instead of one per broadcast --
// videos.list was this project's second-largest quota consumer purely through repetition.
//
// `out` is keyed by video id and holds ONLY the ids the response actually returned. An id
// YouTube omits (ended, deleted, not visible to this account) must stay ABSENT rather than
// appear as 0: downstream, absent means "no figure" while 0 means "live with no viewers", and
// collapsing the two would report a dead broadcast as a live one with an empty audience.
// Attribution is by the item's own id, never by response order, which the API does not
// promise to preserve.
bool YouTubeProvider::ReadBroadcastViewers(OAuthAccount &acct, const std::vector<std::string> &broadcastIds,
					   std::map<std::string, int> &out, std::string &err)
{
	if (broadcastIds.empty()) {
		return true;
	}

	std::string ids;
	for (const std::string &id : broadcastIds) {
		if (!ids.empty()) {
			ids += ",";
		}
		ids += id;
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kVideosUrl) + "?part=liveStreamingDetails&id=" + Http::UrlEncode(ids);

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "YouTube videos request failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	const json parsed = ParseJson(resp.body);
	if (!parsed.is_object() || !parsed.contains("items") || !parsed["items"].is_array()) {
		return true; // no items -> every requested id stays absent
	}
	for (const json &item : parsed["items"]) {
		if (!item.is_object()) {
			continue;
		}
		const std::string videoId = Str(item, "id");
		if (videoId.empty()) {
			continue; // unattributable to a destination; dropping it keeps that one absent
		}
		int count = 0;
		if (item.contains("liveStreamingDetails") && item["liveStreamingDetails"].is_object()) {
			// concurrentViewers is a STRING in the API (absent before/after the live
			// window). Parse tolerantly; absent/garbage -> 0.
			const std::string cv = Str(item["liveStreamingDetails"], "concurrentViewers");
			if (!cv.empty()) {
				try {
					count = std::stoi(cv);
				} catch (const std::exception &) {
					count = 0;
				}
			}
		}
		out[videoId] = count < 0 ? 0 : count;
	}
	return true;
}

bool YouTubeProvider::viewerCounts(OAuthAccount &acct, std::map<DestinationId, int> &out, std::string &err)
{
	const std::string accountId = AccountId(acct);

	// Snapshot this account's live destinations under the lock, then release it: the reads
	// below block on HTTP and broadcastMutex_ must never be held across a network call (it
	// also guards chatChannelRef and the go-live commit).
	auto snapshotTargets = [&] {
		std::map<DestinationId, std::string> targets; // destination -> broadcastId
		const std::lock_guard<std::mutex> guard(broadcastMutex_);
		for (const auto &entry : broadcasts_) {
			if (entry.first.accountId == accountId && !entry.second.broadcastId.empty()) {
				targets[entry.first] = entry.second.broadcastId;
			}
		}
		return targets;
	};

	std::map<DestinationId, std::string> targets = snapshotTargets();

	// Nothing cached (a restart mid-stream, or a go-live this session never applied): ONE
	// probe repopulates every destination of this account at once, each attributed exactly
	// by the ingest stream its broadcast is bound to. One unit regardless of how many
	// orientations come back, and it is throttled, so an off-air account cannot spin on it.
	if (targets.empty()) {
		if (!ProbeActiveBroadcasts(acct, err)) {
			return false;
		}
		targets = snapshotTargets();
		if (targets.empty()) {
			return false;
		}
	}

	// DISTINCT broadcast ids, in one batch. The dedupe is belt-and-braces against two
	// destinations naming one broadcast (only reachable if two profiles somehow remembered the
	// same ingest stream): reporting it twice would double-count those viewers in the caller's
	// sum. The first destination claiming an id keeps it, matching the previous behavior.
	std::vector<std::pair<DestinationId, std::string>> claims; // one entry per distinct broadcast
	std::set<std::string> seen;
	for (const auto &target : targets) {
		if (!seen.insert(target.second).second) {
			continue; // this broadcast is already represented by an earlier destination
		}
		claims.emplace_back(target.first, target.second);
	}

	// Chunked because videos.list caps its id list (50 per request, per the API reference);
	// with one broadcast per live orientation a real setup never reaches one chunk, but a
	// caller cannot be made to care. A chunk that fails leaves ITS ids absent from `counts`
	// and therefore its destinations absent from `out`, exactly as a single failed read did.
	std::map<std::string, int> counts;
	for (size_t offset = 0; offset < claims.size(); offset += kVideosListIdCap) {
		const size_t end = std::min(offset + kVideosListIdCap, claims.size());
		std::vector<std::string> chunk;
		chunk.reserve(end - offset);
		for (size_t i = offset; i < end; ++i) {
			chunk.push_back(claims[i].second);
		}
		std::string readErr;
		if (!ReadBroadcastViewers(acct, chunk, counts, readErr) && err.empty()) {
			// One chunk failing (or the quota gate refusing) must not discard the ids that
			// did read -- keep the first error for the caller's log and carry on, so a
			// partial total still beats no total at all.
			err = readErr;
		}
	}

	bool any = false;
	for (const auto &claim : claims) {
		const auto found = counts.find(claim.second);
		if (found == counts.end()) {
			continue; // not returned -> no figure for this destination; leave it absent
		}
		out[claim.first] = found->second;
		any = true;
	}
	// `err` is deliberately LEFT SET on a partial read: returning true says "these rows are
	// usable", and the caller reports a non-empty err alongside them so a silently-dropped
	// broadcast is still visible in the log. It is cleared only when nothing failed.
	return any;
}

bool YouTubeProvider::audienceCount(OAuthAccount &acct, AudienceResult &out, std::string &err)
{
	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kChannelsUrl) + "?part=statistics&mine=true";

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "YouTube channels request failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	const json item = FirstItem(ParseJson(resp.body));
	if (!item.is_object() || !item.contains("statistics") || !item["statistics"].is_object()) {
		err = "YouTube channels response missing statistics";
		return false;
	}

	const json &stats = item["statistics"];
	out.kind = AudienceKind::Subscribers;
	out.hidden = Bool(stats, "hiddenSubscriberCount", false);
	if (out.hidden) {
		out.count = -1;
	} else {
		// subscriberCount is a STRING in the API, API-rounded to 3 sig figs above 1000.
		try {
			out.count = std::stoll(Str(stats, "subscriberCount"));
		} catch (const std::exception &) {
			out.available = false;
			err = "YouTube subscriberCount was not a parseable number";
			return false;
		}
	}
	out.available = true; // a successful read is authoritative even when hidden
	return true;
}

} // namespace OAuth
