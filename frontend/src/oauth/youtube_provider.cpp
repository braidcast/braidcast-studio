#include "youtube_provider.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string_view>
#include <utility>

#include "../chat/youtube_chat.hpp"
#include "account_store.hpp"
#include "util/env_config.hpp"
#include "util/http_client.hpp"
#include "../ingest_writeback.hpp"
#include "util/innertube_client.hpp"
#include "util/json_util.hpp"
#include "util/op_error.hpp"
#include "util/string_util.hpp"
#include "util/time_util.hpp"
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

// snippet.tags takes arbitrary strings and caps neither how many there are nor how long
// any one of them is -- the only limit is on the characters they add up to.
constexpr int kMaxTagTotalChars = 500;

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

// Units of the shared 10,000/day pool this install will spend on YouTube's CHARGED chat reads
// (the liveChatMessages surfaces) before it stops reading chat for the day. It is a floor under
// a path measured at ~1,730 units per chat-hour PER DESTINATION -- unbounded, four destinations
// spend the whole pool, for every install, inside 87 minutes. 2,000 is a fifth of the pool: enough
// that a private broadcast still gets chat, small enough that one session cannot take the day from
// everyone else. BRAIDCAST_YOUTUBE_CHAT_BUDGET overrides it for diagnosis; 0 refuses the charged
// path outright.
constexpr int kChatUnitBudgetDefault = 2000;

// Slack past the computed midnight before requests resume, so a wake on a
// slightly-fast local clock cannot re-observe quotaExceeded and re-arm the gate
// for a full extra day.
constexpr int64_t kQuotaResetSlackSec = 300;

// Furthest ahead of now a RESTORED reset instant can plausibly sit. A real one is the next
// midnight Pacific computed at some moment in the past, so it is at most ~24h out; 26h
// covers a DST shift plus the slack above. Anything beyond is corrupt or clock-skewed and is
// discarded rather than honored -- see EnsureQuotaStateLoaded for why failing open is right.
constexpr int64_t kQuotaResetHorizonSec = 26 * 60 * 60;

// The InnerTube endpoint the logged-out watch page reads its live viewer figure from. NOT a
// Data API URL and NOT sent through SendAuthed: anonymous, zero-quota, and built by
// util/innertube_client, which owns the compliance rule for it. prettyPrint=false because the
// response is machine-read and the indentation is pure upload cost.
const char *kUpdatedMetadataUrl = "https://www.youtube.com/youtubei/v1/updated_metadata?prettyPrint=false";

using JsonUtil::Bool;
using JsonUtil::CopyString;
using JsonUtil::First;
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

// Read ONCE: this is a start-up knob, and the budget is consulted on every billed chat request
// (several a minute per destination), where Env::Number's fallback to the repo-root .env would
// be a file open per request.
int ChatUnitBudget()
{
	static const int budget =
		static_cast<int>(Env::Number("BRAIDCAST_YOUTUBE_CHAT_BUDGET", kChatUnitBudgetDefault));
	return budget;
}

// An epoch instant as local wall-clock "HH:MM" ("" for the never-set 0). Shared by the quota
// gate's message and the chat budget's, so the two cannot render the same reset differently.
std::string LocalHhMm(int64_t epoch)
{
	if (epoch == 0) {
		return std::string();
	}
	const std::time_t t = static_cast<std::time_t>(epoch);
	std::tm tm{};
	localtime_s(&tm, &t);
	char buf[8];
	std::strftime(buf, sizeof buf, "%H:%M", &tm);
	return std::string(buf);
}

// A short content digest for change detection -- not a security primitive. The byte length is
// folded in so two payloads of different sizes cannot collide on the hash alone, which is what
// makes it safe to keep INSTEAD of the bytes it summarizes (the thumbnail's).
std::string ContentDigest(std::string_view bytes)
{
	return std::to_string(bytes.size()) + ":" + std::to_string(std::hash<std::string_view>{}(bytes));
}

using StringUtil::ContainsCI;

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

// The videoViewCountRenderer out of an updated_metadata payload, or a null json when the
// response carried no viewership update at all.
//
// Found by SCANNING actions[] for updateViewershipAction, never by index. Index 0 is where it
// was observed, but actions[] is a list of whatever changed on the watch page this tick, so its
// composition is not something to depend on -- and indexing blindly would read some other
// action's fields as a viewer count.
const json &ViewershipRenderer(const json &payload)
{
	static const json kNull = json(nullptr);
	const json &actions = Obj(payload, "actions");
	if (!actions.is_array()) {
		return kNull;
	}
	for (const json &action : actions) {
		const json &viewCount = Obj(Obj(action, "updateViewershipAction"), "viewCount");
		const json &renderer = Obj(viewCount, "videoViewCountRenderer");
		if (renderer.is_object()) {
			return renderer;
		}
	}
	return kNull;
}

// Beyond any plausible concurrent audience; a value past this is a misread field rather than a
// number, and refusing it leaves the count absent instead of truncating into `int`.
constexpr int64_t kMaxPlausibleViewers = 1000000000;

// YouTube's public concurrent figure has NO ZERO STATE while a broadcast is live, so it always
// overstates a quiet stream by this much. Measured across 60 unrelated live broadcasts: not one
// reported 0, and the low end sat at exactly 1 ("1 watching now"). Confirmed against our own
// broadcasts from the other direction -- four of them read a steady 1 for a whole stream whose
// CUMULATIVE view total was still "No views" afterwards, so the 1 corresponded to no playback
// session at all.
//
// Reporting it verbatim tells a streamer an audience is present when nobody is watching, which
// is the worst available direction to be wrong in: they start talking to an empty room. Removed
// here, where the figure enters the app, so the dock, the aggregate and the overlays cannot
// disagree about it -- and never in a view, which would leave every other consumer overstating.
//
// Whether YouTube clamps (max(1, true)) or offsets (true + 1) is NOT yet measured, and it is the
// difference between this being exact and a lone real viewer reading 0. The residual error is one
// viewer at the very bottom of the range, and it understates rather than invents an audience. To
// settle it: go live public and open exactly one known viewer on the watch page -- 2 means offset
// (this is exact), 1 means clamp (subtracting here costs that one viewer).
constexpr int kYouTubeLiveViewerFloor = 1;

// A viewer figure out of either the raw unlocalized integer string ("7175") or a display string
// whose group separators have to come off first ("7,175"). False -- leaving `out` untouched --
// for anything that is not purely digits and group separators, which is what keeps localized
// prose ("7,175 watching now", "7,1 mil") from ever being mistaken for a number: the count then
// stays ABSENT, which is a correct answer, where a partial parse would be a wrong one.
bool ParseViewerCount(const std::string &text, int &out)
{
	int64_t value = 0;
	bool digits = false;
	for (const char c : text) {
		if (c >= '0' && c <= '9') {
			value = value * 10 + (c - '0');
			digits = true;
			if (value > kMaxPlausibleViewers) {
				return false;
			}
			continue;
		}
		// Group separators only. Anything else -- a letter, a currency mark, a multi-byte
		// space -- means this string is not a bare number, so refuse the whole thing.
		if (c != ',' && c != '.' && c != ' ') {
			return false;
		}
	}
	if (!digits) {
		return false;
	}
	out = static_cast<int>(value);
	return true;
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
	return LocalHhMm(quotaResetEpoch_.load(std::memory_order_acquire));
}

bool YouTubeProvider::ChargeChatUnits(int units)
{
	const int budget = ChatUnitBudget();
	std::string exhaustedAt; // host-logged after the lock, never while holding it

	bool ok = false;
	{
		const std::lock_guard<std::mutex> guard(chatBudgetMutex_);
		const int64_t now = static_cast<int64_t>(std::time(nullptr));
		if (now >= chatDayEnd_) {
			// First charge ever, or the first past midnight Pacific: start the day fresh.
			// The same boundary the Data API's own daily quota resets on, so a budget that
			// ran out and a quota that ran out come back together rather than an hour apart.
			chatDayEnd_ = NextPacificMidnightUtc(now);
			chatUnitsSpent_ = 0;
			chatBudgetLogged_ = false;
		}
		ok = units <= 0 || chatUnitsSpent_ + units <= budget;
		if (ok) {
			chatUnitsSpent_ += units;
		} else if (!chatBudgetLogged_) {
			chatBudgetLogged_ = true;
			exhaustedAt = LocalHhMm(chatDayEnd_);
		}
	}

	if (!exhaustedAt.empty()) {
		HostLog("[oauth] YouTube charged-chat budget spent (" + std::to_string(budget) +
			" units); YouTube chat stops until " + exhaustedAt +
			" local. The free reader handles unlisted and public alike, so whatever spent this "
			"budget had its free read fail, had it turned off, or had a privacy that could not "
			"be resolved.");
	}
	return ok;
}

bool YouTubeProvider::ChatBudgetExhausted() const
{
	const int budget = ChatUnitBudget();
	const std::lock_guard<std::mutex> guard(chatBudgetMutex_);
	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	if (now >= chatDayEnd_) {
		return false; // a new day has begun; the next charge will roll it
	}
	return chatUnitsSpent_ >= budget;
}

std::string YouTubeProvider::ChatBudgetMessage() const
{
	std::string resumesAt;
	{
		const std::lock_guard<std::mutex> guard(chatBudgetMutex_);
		resumesAt = LocalHhMm(chatDayEnd_);
	}
	return "YouTube chat stopped: today's YouTube chat allowance is used up" +
	       (resumesAt.empty() ? std::string() : ", so chat resumes after " + resumesAt) + ".";
}

std::string YouTubeProvider::QuotaMessage() const
{
	return "YouTube has paused this app's access until " + QuotaResetLocalTime();
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
		  true, // revokePreferAccessToken -- Google's docs confirm this also revokes the paired refresh token
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

ChatBroadcastRef YouTubeProvider::chatBroadcastRef(OAuthAccount &acct, const std::string &profileUuid)
{
	BroadcastState state;
	std::string err;
	if (EnsureActiveBroadcast(acct, profileUuid, state, err)) {
		return ChatBroadcastRef{state.broadcastId, state.privacy};
	}
	DBG(LogCat::Chat, "youtube: dest=%s has no resolvable broadcast video id (%s)",
	    DestinationKey(DestinationId{AccountId(acct), profileUuid}).c_str(),
	    err.empty() ? "not live on this destination" : err.c_str());
	return ChatBroadcastRef{};
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

std::vector<DestinationId> YouTubeProvider::LiveDestinations(const std::string &accountId) const
{
	std::vector<DestinationId> live;
	const std::lock_guard<std::mutex> guard(broadcastMutex_);
	for (const auto &entry : broadcasts_) {
		if (entry.first.accountId == accountId && !entry.second.broadcastId.empty()) {
			live.push_back(entry.first);
		}
	}
	return live;
}

bool YouTubeProvider::IsAccountBroadcasting(const std::string &accountId) const
{
	return !LiveDestinations(accountId).empty();
}

bool YouTubeProvider::ShouldPollSuperChats(const std::string &accountId) const
{
	// Snapshot the account's live destinations, then release broadcastMutex_ before taking
	// liveChatMutex_: the two are never held together, so no lock order exists to invert.
	const std::vector<DestinationId> liveDestinations = LiveDestinations(accountId);
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
			      {"scope", "all"},
			      {"max", 100}});
	// A videoCategoryId is YouTube's own enumeration; no other platform can resolve one.
	fields.push_back(json{{"key", "category"},
			      {"label", "Category"},
			      {"type", "category"},
			      {"tier", "simple"},
			      {"scope", "provider"}});
	// YouTube accepts arbitrary tag strings where Twitch enforces a character rule, so a
	// value crossing to Twitch would be one Twitch rejects.
	fields.push_back(json{{"key", "tags"},
			      {"label", "Tags"},
			      {"type", "tags"},
			      {"tier", "simple"},
			      {"scope", "provider"},
			      {"maxTotalChars", kMaxTagTotalChars}});
	// Only YouTube takes a thumbnail, and its 2 MB/aspect rules are its own -- so one image
	// serves every YouTube channel the user runs and is picked once for all of them.
	fields.push_back(json{{"key", "thumbnail"},
			      {"label", "Thumbnail"},
			      {"type", "image"},
			      {"tier", "simple"},
			      {"scope", "provider"}});
	fields.push_back(json{{"key", "description"},
			      {"label", "Description"},
			      {"type", "textarea"},
			      {"tier", "simple"},
			      {"scope", "all"}});
	// Privacy defaults to "private": broadcasting publicly must be an explicit
	// choice, never the result of leaving the field untouched. `required` says the
	// field has no valid empty state, so the UI offers no unset option and shows the
	// value applyMetadata below would actually send.
	// optionNotes: the consequence of the CURRENTLY SELECTED value, shown under the control.
	// Only `private` carries one: a logged-out viewer may watch an unlisted broadcast, so the
	// free chat reader sees it exactly as it sees a public one, and only a private broadcast
	// leaves this destination with no chat at all. Said here, at the point of choice, rather
	// than discovered when chat never arrives mid-broadcast.
	fields.push_back(
		json{{"key", "privacy"},
		     {"label", "Privacy"},
		     {"type", "enum"},
		     {"tier", "simple"},
		     {"scope", "provider"},
		     {"required", true},
		     {"default", "private"},
		     {"options", json::array({json{{"value", "public"}, {"label", "Public"}},
					      json{{"value", "unlisted"}, {"label", "Unlisted"}},
					      json{{"value", "private"}, {"label", "Private"}}})},
		     {"optionNotes",
		      json{{"private",
			    "Live chat is not available on private broadcasts. Unlisted and public both work."}}}});
	fields.push_back(json{{"key", "latency"},
			      {"label", "Latency"},
			      {"type", "enum"},
			      {"tier", "advanced"},
			      {"scope", "channel"},
			      {"required", true},
			      {"default", "normal"},
			      {"options", json::array({json{{"value", "normal"}, {"label", "Normal"}},
						       json{{"value", "low"}, {"label", "Low latency"}},
						       json{{"value", "ultraLow"}, {"label", "Ultra-low latency"}}})}});
	fields.push_back(
		json{{"key", "dvr"}, {"label", "DVR"}, {"type", "bool"}, {"tier", "advanced"}, {"scope", "channel"}});
	// The default is declared because applyMetadata always sends this flag, defaulting it to
	// false: without the declaration the read-back has nothing to compare an unset bag against,
	// and a safety field would go unchecked on exactly the destinations that never touched it.
	fields.push_back(json{{"key", "madeForKids"},
			      {"label", "Made for kids"},
			      {"type", "bool"},
			      {"tier", "advanced"},
			      {"scope", "channel"},
			      {"default", false}});
	fields.push_back(json{{"key", "autoStop"},
			      {"label", "Auto-stop when stream ends"},
			      {"type", "bool"},
			      {"tier", "advanced"},
			      {"scope", "channel"},
			      {"default", true}});
	fields.push_back(json{{"key", "projection"},
			      {"label", "360\xC2\xB0"},
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

bool YouTubeProvider::fetchIdentity(OAuthAccount &acct, std::string &err)
{
	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kChannelsUrl) + "?part=snippet&mine=true";

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (!Http::Require2xx(resp, "YouTube channels request", err)) {
		return false;
	}

	const json item = First(ParseJson(resp.body), "items");
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
		if (!Http::Require2xx(resp, "YouTube category list", err)) {
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
	const DestinationId dest{AccountId(acct), profileUuid};

	// Read every field up front (tolerant defaults; YouTube rejects empties on the
	// required fields, so substitute safe values rather than send "").
	std::string title = Str(fields, "title");
	if (title.empty()) {
		title = kDefaultTitle;
	}
	const std::string description = Str(fields, "description");
	std::string privacy = Str(fields, "privacy");
	if (privacy != kPrivacyPublic && privacy != kPrivacyUnlisted && privacy != kPrivacyPrivate) {
		// Last-resort guard only -- the real default is the capability field's
		// remembered-else-private (seeded by the modal). An absent/invalid value
		// must resolve to the safest visibility; it must never silently go public.
		privacy = kPrivacyPrivate;
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

	// The ONE test for "this exact payload is already on that video id". `skipUnchanged` is
	// the mid-stream path's flag alone: a go-live creates a BRAND NEW broadcast, hence a new
	// video id that owns none of this yet, so nothing there is ever redundant.
	//
	// Recording happens on SUCCESS only, so a failed send always re-sends next time -- the
	// safe direction. Deliberately blind to server-side drift: a change made in YouTube
	// Studio's own UI is not seen here, so Apply will not re-assert over it until the value
	// in the modal changes. That is the accepted cost of not re-uploading a 2 MB thumbnail
	// every time the user fixes a typo.
	auto alreadyApplied = [&](const std::string &videoId, AppliedKind kind, const std::string &digest,
				  bool skipUnchanged) {
		return skipUnchanged && !digest.empty() && AppliedDigest(dest, videoId, kind) == digest;
	};

	// Steps 4 + 5 (video category/tags, then thumbnail) apply identically whether the
	// broadcast was just created for go-live or is already live for a mid-stream edit, so
	// both paths call this instead of duplicating the blocks. Both are NON-CRITICAL:
	// failures are logged and skipped, never surfaced as an apply failure.
	auto applyVideoTagsAndThumbnail = [&](const std::string &videoId, bool skipUnchanged) {
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
			const std::string digest = ContentDigest(videoSnippet.dump());
			if (alreadyApplied(videoId, AppliedKind::Snippet, digest, skipUnchanged)) {
				DBG(LogCat::OAuth, "youtube: dest=%s videos.update skipped (unchanged, 50 units saved)",
				    DestinationKey(dest).c_str());
			} else {
				json vResp;
				std::string vErr;
				if (!sendJson("PUT", std::string(kVideosUrl) + "?part=snippet", videoBody, vResp,
					      vErr)) {
					HostLog("[oauth] YouTube videos.update failed (continuing): " +
						Err::Diagnostic(vErr));
				} else {
					RecordApplied(dest, videoId, AppliedKind::Snippet, digest);
				}
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
				// The digest is taken from the BYTES, not the path: re-picking the same
				// image from a different folder is not a change, and editing a file in
				// place under an unchanged path is. Only the digest is kept.
				const std::string digest = ContentDigest(bytes);
				if (bytes.empty()) {
					HostLog("[oauth] YouTube thumbnail skipped: empty file " + thumbnailPath);
				} else if (alreadyApplied(videoId, AppliedKind::Thumbnail, digest, skipUnchanged)) {
					DBG(LogCat::OAuth,
					    "youtube: dest=%s thumbnails.set skipped (identical image, 50 units saved)",
					    DestinationKey(dest).c_str());
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
					} else {
						RecordApplied(dest, videoId, AppliedKind::Thumbnail, digest);
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
			json existing;
			if (!ReadBroadcast(acct, active.broadcastId, existing, err)) {
				return false;
			}
			std::string scheduledStart = Str(Obj(existing, "snippet"), "scheduledStartTime");
			if (scheduledStart.empty()) {
				scheduledStart = TimeUtil::NowIso8601Utc();
			}

			// contentDetails (latency/dvr/autoStart) is intentionally omitted: those are
			// go-live-time settings, immutable once the broadcast is testing/live (a PUT
			// including them would 400).
			json updSnippet = json{{"title", title},
					       {"description", description},
					       {"scheduledStartTime", scheduledStart}};
			json updStatus = json{{"privacyStatus", privacy}, {"selfDeclaredMadeForKids", madeForKids}};
			json updBody = json{{"id", active.broadcastId}, {"snippet", updSnippet}, {"status", updStatus}};
			// The same 50-unit re-send the video/thumbnail steps below avoid, and for the
			// same reason: the read above already told us the broadcast's current
			// scheduledStartTime, so an unchanged payload is a write of what is already there.
			const std::string updDigest = ContentDigest(updBody.dump());
			if (alreadyApplied(active.broadcastId, AppliedKind::Broadcast, updDigest, true)) {
				DBG(LogCat::OAuth,
				    "youtube: dest=%s liveBroadcasts.update skipped (unchanged, 50 units saved)",
				    DestinationKey(dest).c_str());
			} else {
				json updResp;
				std::string updErr;
				if (!sendJson("PUT", std::string(kLiveBroadcastsUrl) + "?part=snippet,status", updBody,
					      updResp, updErr)) {
					err = Err::Wrap("YouTube liveBroadcasts.update failed: ", updErr);
					return false;
				}
				RecordApplied(dest, active.broadcastId, AppliedKind::Broadcast, updDigest);
			}

			// The cached privacy is only as good as the last value this app sent, so refresh
			// it here too: this entry is what the chat transport re-reads before it enters a
			// charged read, so a mid-stream switch to private reaches that gate through this
			// write. Existing entry only, so a destination that went off air mid-edit is not
			// resurrected by its own late record.
			{
				const std::lock_guard<std::mutex> guard(broadcastMutex_);
				auto it = broadcasts_.find(dest);
				if (it != broadcasts_.end()) {
					it->second.privacy = privacy;
				}
			}

			applyVideoTagsAndThumbnail(active.broadcastId, true);
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

	// Anonymous InnerTube -- the zero-quota chat reader -- reads anything a logged-out viewer
	// may watch, so unlisted costs nothing and only PRIVATE is invisible to it. A private
	// broadcast therefore gets no chat at all: the only surface that could read it bills
	// ~1,730 units/hour against the pool every install shares, for the broadcast's whole
	// duration, and the chat transport refuses that outright. Said here, at the moment the
	// choice takes effect, because it is the log line that explains a destination whose chat
	// never arrives. The privacy DEFAULT is deliberately not changed to buy chat: broadcasting
	// beyond private stays an explicit choice.
	if (privacy == kPrivacyPrivate) {
		HostLog("[oauth] YouTube dest=" + DestinationKey(dest) +
			" is going live private; YouTube's free chat reader cannot see a private broadcast, so "
			"this destination will have no chat. Unlisted reads chat free, exactly as public does");
	}

	// 1. liveBroadcasts.insert -- the broadcast id doubles as the videoId. CRITICAL.
	json snippet =
		json{{"title", title}, {"description", description}, {"scheduledStartTime", TimeUtil::NowIso8601Utc()}};
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
			const json item = First(ParseJson(chatResp.body), "items");
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
	std::string ingestionType;

	// A liveStream's cdn.ingestionType is fixed when the resource is created and cannot be
	// changed afterwards, so it is part of the resource's IDENTITY rather than one of its
	// settings: a stream created for one protocol is not a stream this profile can use once
	// the profile targets the other. YouTube validates the incoming feed against this field,
	// not against the port the bytes arrived on -- an AV1 feed pushed over RTMPS to a
	// broadcast bound to an hls-typed stream was refused with "The video is encoded with an
	// unsupported codec ... (H.264)", because under HLS rules that is true.
	//
	// "HLS" is the only protocol that maps anywhere but rtmp; RTMP and RTMPS share one
	// ingestion type, and an unknown/absent protocol falls back to rtmp because that is what
	// every YouTube destination that is not explicitly HLS uses.
	const std::string profileProtocol = ReadProfileIngestProtocol(profileUuid);
	const std::string requiredIngestionType = StringUtil::EqualsCI(profileProtocol, "HLS") ? "hls" : "rtmp";

	// Pulls id + cdn.ingestionInfo out of a liveStreams resource; false when any of
	// the three fields the RTMP output needs is missing. ingestionType comes from the same
	// response -- the verify query below already asks for `cdn`, so having it costs nothing.
	auto readIngestion = [&](const json &item) -> bool {
		streamId = Str(item, "id");
		ingestionAddress.clear();
		streamName.clear();
		ingestionType.clear();
		if (item.is_object() && item.contains("cdn") && item["cdn"].is_object()) {
			const json &cdn = item["cdn"];
			ingestionType = Str(cdn, "ingestionType");
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
	// Why the remembered stream was dropped, when it was dropped for a reason more specific
	// than "the API would not confirm it". Empty means the generic case.
	std::string discardReason;
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
		verifyReq.url =
			std::string(kLiveStreamsUrl) + "?part=id,cdn,status&id=" + Http::UrlEncode(rememberedStreamId);
		Http::HttpResponse verifyResp;
		std::string verifyErr;
		if (SendAuthed(acct, verifyReq, verifyResp, verifyErr) && verifyResp.status >= 200 &&
		    verifyResp.status < 300) {
			const json item = First(ParseJson(verifyResp.body), "items");
			const bool errored = item.is_object() && item.contains("status") &&
					     item["status"].is_object() &&
					     Str(item["status"], "streamStatus") == "error";
			if (!errored && readIngestion(item) && streamId == rememberedStreamId) {
				// The type check belongs in the reuse predicate rather than in a
				// repair path of its own: a wrong-typed stream is simply not a
				// cache hit, so it falls through to the insert below that already
				// handles "no remembered stream survived". Nothing has to detect
				// or migrate the stale resource.
				if (!ingestionType.empty() && ingestionType != requiredIngestionType) {
					discardReason = "is " + ingestionType + " but this profile now needs " +
							requiredIngestionType + " (protocol " +
							(profileProtocol.empty() ? "unknown" : profileProtocol) + ")";
					streamId.clear();
				} else {
					HostLog("[oauth] YouTube reusing ingest stream " + streamId);
				}
			} else {
				streamId.clear();
			}
		}
		if (streamId.empty()) {
			// Two different discards reach here and they are not the same event: a stream
			// that could not be verified at all, and one verified fine but built for the
			// other protocol. Saying "not verified" for the second would send the next
			// reader looking for a network fault that never happened.
			HostLog("[oauth] YouTube remembered ingest stream " + rememberedStreamId + " " +
				(discardReason.empty() ? "not verified" : discardReason) + "; creating a fresh one");
		}
	}

	if (streamId.empty()) {
		// A fixed name, not the broadcast title: this resource outlives the broadcast
		// that happened to create it and shows up in the channel's stream list, where
		// one go-live's title would be a confusing label for every later one.
		json streamBody = json{
			{"snippet", json{{"title", "Braidcast ingest"}}},
			// From the profile's protocol, not a constant: a hardcoded "rtmp" here is how
			// an HLS profile ended up bound to an rtmp-typed stream and vice versa, and
			// the type cannot be changed after creation.
			{"cdn", json{{"frameRate", "variable"},
				     {"ingestionType", requiredIngestionType},
				     {"resolution", "variable"}}},
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

	// One assertion covering BOTH paths above -- the reused stream and the freshly inserted
	// one -- placed here because this is the last point where the stream is still just a
	// resource and no broadcast is bound to it yet.
	//
	// This compares two values YouTube itself declared, not the shape of a URL, so it is safe
	// to refuse on: if it ever trips, pushing anyway is guaranteed to produce a broadcast that
	// ingests and never goes live, which is the single most expensive failure this integration
	// has (the encoder reports success, the upload is accepted, and the only symptom is on the
	// dashboard). A refused go-live with the reason named costs a retry; the silent version
	// cost an afternoon.
	if (!ingestionType.empty() && ingestionType != requiredIngestionType) {
		err = "YouTube ingest stream " + streamId + " accepts " + ingestionType +
		      " but this destination streams " +
		      (profileProtocol.empty() ? "an unknown protocol" : profileProtocol) + ", which needs " +
		      requiredIngestionType +
		      ". YouTube would judge the feed by the wrong protocol's rules and hold the "
		      "broadcast at 'waiting for ingest'";
		deleteOrphanBroadcast(broadcastId);
		return false;
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
	// committed once this apply fully succeeds (below). Reset WHOLE rather than field by
	// field: every per-broadcast cache in here (the viewer continuation, the re-apply
	// digests) belongs to the broadcast being replaced, and a field-by-field clear is how one
	// of them survives into the next broadcast the day another is added.
	{
		const std::lock_guard<std::mutex> guard(broadcastMutex_);
		broadcasts_[dest] = BroadcastState{};
	}

	// 4 + 5. Video category/tags, then thumbnail (both NON-CRITICAL). Shared with the
	// mid-stream edit path, but never skipping: this video id was created seconds ago and
	// carries none of it yet.
	applyVideoTagsAndThumbnail(broadcastId, false);

	// 6. Ingest writeback -- put the CDN endpoint + key into the linked profile so
	// the modal's streaming.start streams to YouTube. Blocks on the UI-thread write
	// so the key is present before the caller triggers go-live. CRITICAL.
	// Only the part before the query: for HLS the address carries the stream key as a query
	// parameter, and no key belongs in a log file. The SHAPE is the part worth having --
	// the output does not push to this address as returned, rtmp_common rebuilds the URL
	// from the service's own template, so an address differing in host or path would be
	// discarded with nothing anywhere to show it.
	HostLog("[oauth] YouTube dest=" + DestinationKey(dest) + " bound broadcast " + broadcastId +
		" to ingest stream " + streamId + ", API ingest address " +
		ingestionAddress.substr(0, ingestionAddress.find('?')));
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
		// The privacy this insert just sent, so the chat transport can tell a private
		// broadcast (no free reader, and no charged one either) from one it may read for
		// free -- without spending a request to ask what we ourselves chose.
		bs.privacy = privacy;
	}
	return true;
}

bool YouTubeProvider::ReadBroadcast(OAuthAccount &acct, const std::string &broadcastId, json &item, std::string &err)
{
	item = json(nullptr);
	if (broadcastId.empty()) {
		err = "YouTube broadcast id is empty";
		return false;
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kLiveBroadcastsUrl) + "?part=snippet,status&id=" + Http::UrlEncode(broadcastId);

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (!Http::Require2xx(resp, "YouTube liveBroadcasts.list", err)) {
		return false;
	}

	item = First(ParseJson(resp.body), "items");
	if (!item.is_object()) {
		err = "YouTube reported no broadcast " + broadcastId;
		return false;
	}
	return true;
}

bool YouTubeProvider::reapplyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields,
				      std::string &err)
{
	// Drop this destination's broadcast-resource memo so the push below is actually sent: the edit
	// path skips a liveBroadcasts.update whose payload matches the memo, and a corrective push
	// repeats that payload exactly. Where a memo exists it has just been contradicted by what the
	// platform reported, so it is no longer describing the broadcast; where none exists -- a
	// broadcast this go-live created, which records no update digest -- the clear costs nothing.
	// Only the broadcast entry: neither the video snippet nor the thumbnail carries a field
	// readAppliedMetadata reports, so invalidating those memos would put 100 units behind values
	// no divergence can ever name.
	{
		const std::lock_guard<std::mutex> guard(broadcastMutex_);
		auto it = broadcasts_.find(DestinationId{AccountId(acct), profileUuid});
		if (it != broadcasts_.end()) {
			it->second.appliedBroadcast.clear();
		}
	}
	return StreamProvider::reapplyMetadata(acct, profileUuid, fields, err);
}

bool YouTubeProvider::readAppliedMetadata(OAuthAccount &acct, const std::string &profileUuid, AppliedBy by,
					  AppliedState &out, std::string &err)
{
	(void)by; // a YouTube broadcast reports its own snippet and status the same way either way
	out = AppliedState{};
	BroadcastState active;
	if (!EnsureActiveBroadcast(acct, profileUuid, active, err)) {
		if (err.empty()) {
			// EnsureActiveBroadcast leaves `err` empty for "nothing live here", which
			// includes a throttled probe. Neither is a reason to claim agreement.
			err = "YouTube reports no live broadcast for this destination";
		}
		return false;
	}

	json item;
	if (!ReadBroadcast(acct, active.broadcastId, item, err)) {
		return false;
	}
	const json &snippet = Obj(item, "snippet");
	const json &status = Obj(item, "status");

	CopyString(snippet, "title", out.fields, "title");
	CopyString(snippet, "description", out.fields, "description");
	// A response that carried no privacyStatus leaves the field out, so the diff has nothing to
	// compare -- and says so, because privacy is a safety field: an invented empty visibility
	// would disagree with whatever was asked for and refuse a go-live over a value YouTube never
	// stated, while silence on it would read as agreement.
	if (!CopyString(status, "privacyStatus", out.fields, "privacy")) {
		out.unconfirmed = displayName() + " did not report the visibility of this broadcast";
	}
	// Presence-preserving: a status that did not state the flag must read as unknown rather
	// than as off, or a destination that genuinely asked for made-for-kids would be refused
	// over a field YouTube simply did not echo.
	const json &kids = Obj(status, "selfDeclaredMadeForKids");
	if (kids.is_boolean()) {
		out.fields["madeForKids"] = kids.get<bool>();
	}
	// Category, tags and the thumbnail live on the VIDEO resource, not on the broadcast, so
	// reading them would be a SECOND billed request per destination per go-live on the one
	// platform where daily quota is the binding constraint. They are left out, and the diff
	// compares only what is here.
	return true;
}

bool YouTubeProvider::hasActiveBroadcast(OAuthAccount &acct, const std::string &profileUuid)
{
	BroadcastState active;
	// The probe error is deliberately swallowed: "could not tell" has to read as "not
	// ready", so the caller creates a broadcast rather than starting an output against
	// nothing. A genuine API failure then surfaces from the create, with its own message.
	std::string probeErr;
	return EnsureActiveBroadcast(acct, profileUuid, active, probeErr);
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

	// contentDetails carries boundStreamId and status carries privacyStatus, and
	// liveBroadcasts.list bills the same 1 unit whichever parts are requested -- so the
	// attribution below, and learning whether the free chat reader may see each broadcast,
	// are both free.
	//
	// `broadcastStatus` WITHOUT `mine`: liveBroadcasts.list takes exactly one filter and
	// rejects the pair with "Incompatible parameters specified in the request: mine,
	// broadcastStatus" (HTTP 400), which silently left every destination without a resolved
	// broadcast. Do not re-add `mine=true`. Filtering by status is the better half to keep:
	// `mine` alone would return completed and upcoming broadcasts too, so an account with
	// more than maxResults of history could push its live one off the page. Should the status
	// filter ever return broadcasts this account does not own, the boundStreamId attribution
	// below still only accepts one bound to a stream this account remembers.
	Http::HttpReq req;
	req.method = "GET";
	req.url = std::string(kLiveBroadcastsUrl) +
		  "?part=id,snippet,status,contentDetails&broadcastStatus=active&maxResults=50";

	Http::HttpResponse resp;
	if (!SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (!Http::Require2xx(resp, "YouTube liveBroadcasts request", err)) {
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
		if (item.contains("status") && item["status"].is_object()) {
			// The authoritative answer for a broadcast this session did not create (a
			// Studio restart mid-stream), where there is no remembered choice to fall
			// back on.
			bs.privacy = Str(item["status"], "privacyStatus");
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
		HostLog("[oauth] YouTube skipped " + std::to_string(unattributed) + " active broadcast(s) for " +
			accountId + " bound to an unrecognized ingest stream");
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
			} else if (slot.broadcastId == entry.second.broadcastId && slot.privacy.empty() &&
				   !entry.second.privacy.empty()) {
				// Backfill rather than overwrite: an entry that holds a broadcast id but
				// no privacy would read as unknown for the rest of the broadcast, and
				// unknown is the value the charged chat read is allowed to proceed on.
				// Matching ids first keeps the staleness above out of it -- a just-ended
				// broadcast on the same ingest stream must not lend its privacy to the
				// live one.
				slot.privacy = entry.second.privacy;
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

std::string YouTubeProvider::AppliedDigest(const DestinationId &dest, const std::string &videoId,
					   AppliedKind kind) const
{
	if (videoId.empty()) {
		return std::string();
	}
	const std::lock_guard<std::mutex> guard(broadcastMutex_);
	auto it = broadcasts_.find(dest);
	if (it == broadcasts_.end() || it->second.appliedVideoId != videoId) {
		return std::string(); // nothing remembered for THIS video -> the caller must send
	}
	return it->second.Digest(kind);
}

void YouTubeProvider::RecordApplied(const DestinationId &dest, const std::string &videoId, AppliedKind kind,
				    const std::string &digest)
{
	if (videoId.empty()) {
		return;
	}
	const std::lock_guard<std::mutex> guard(broadcastMutex_);
	auto it = broadcasts_.find(dest);
	if (it == broadcasts_.end()) {
		return; // the destination went off air while this apply was in flight
	}
	BroadcastState &bs = it->second;
	if (bs.appliedVideoId != videoId) {
		// A different broadcast owns the slot: drop its record wholesale rather than leave
		// one resource's digest standing next to another's.
		bs.appliedVideoId = videoId;
		bs.appliedBroadcast.clear();
		bs.appliedSnippet.clear();
		bs.appliedThumbnail.clear();
	}
	bs.Digest(kind) = digest;
}

std::string YouTubeProvider::ViewerContinuation(const DestinationId &dest) const
{
	const std::lock_guard<std::mutex> guard(broadcastMutex_);
	auto it = broadcasts_.find(dest);
	return it == broadcasts_.end() ? std::string() : it->second.viewerContinuation;
}

void YouTubeProvider::SetViewerContinuation(const DestinationId &dest, const std::string &token)
{
	const std::lock_guard<std::mutex> guard(broadcastMutex_);
	auto it = broadcasts_.find(dest);
	if (it == broadcasts_.end()) {
		return; // the destination went off air while this read was in flight
	}
	it->second.viewerContinuation = token;
}

// Read ONE broadcast's concurrent viewers from InnerTube's updated_metadata -- the endpoint the
// logged-out watch page itself polls, at zero Data API quota. One POST per id: unlike
// videos.list this endpoint has no batch form, and with one broadcast per live orientation
// there is nothing worth batching anyway.
//
// THE isLive GUARD IS THE WHOLE CORRECTNESS OF THIS READ. videoViewCountRenderer carries
// isLive:true only while its figure is CONCURRENT viewers; without that flag the very same
// originalViewCount field is the video's CUMULATIVE view total (measured at 1.79 billion on a
// VOD) -- a plausible-looking number nothing downstream would reject, which makes reading it
// unguarded the worst available failure. On an ENDED stream originalViewCount was "0" while the
// display text still held the real total, so the field is trustworthy ONLY under isLive:true.
// yt-dlp branches on exactly this flag (youtube/_video.py). No isLive -> the id stays ABSENT.
//
// `out` gains an entry ONLY for a live renderer whose count actually parsed. Absent means "no
// figure" and 0 means "live with no viewers"; collapsing the two would report a dead broadcast
// as a live one with an empty audience, so no path here ever writes a fallback 0.
//
// An InnerTube failure is NOT a Data API quota failure: nothing here goes through SendAuthed, so
// it cannot arm the quota gate, and the gate being closed cannot refuse it. The two surfaces are
// now independent and must stay so.
bool YouTubeProvider::ReadBroadcastViewers(const DestinationId &dest, const std::string &videoId,
					   std::map<std::string, int> &out, std::string &err)
{
	if (videoId.empty()) {
		return true;
	}
	const std::string destTag = DestinationKey(dest);

	// Two attempts at most: a resumed read that comes back with nothing to update re-reads once
	// from the video id, which re-issues a token. Anything past that leaves the id absent.
	std::string continuation = ViewerContinuation(dest);
	for (int attempt = 1; attempt <= 2; ++attempt) {
		const bool resumed = !continuation.empty();
		// Which request shape went out, not how big the answer was: this prints before the
		// POST, so any size here is a guess. The [net] line that follows carries the real
		// byte count -- and a stated expectation that disagrees with it reads as a
		// measurement and sends you looking in the wrong place.
		DBG(LogCat::OAuth, "youtube viewers: dest=%s continuation cache %s", destTag.c_str(),
		    resumed ? "hit (resuming)" : "miss (full videoId read)");

		const json fields = resumed ? json{{"continuation", continuation}} : json{{"videoId", videoId}};
		const InnerTube::Result resp = InnerTube::Post(kUpdatedMetadataUrl, fields);
		if (resp.canceled) {
			return true; // nothing was sent; this destination simply has no figure this pass
		}
		if (!resp.ok()) {
			err = "YouTube viewer read failed (HTTP " + std::to_string(resp.status) + ")" +
			      (resp.error.empty() ? std::string() : ": " + resp.error);
			return false;
		}

		// The token for the next poll, whatever this payload turns out to carry. On a live
		// broadcast timeoutMs is 5000 -- read but not obeyed: the poll cadence belongs to
		// ViewerPoller (20s, already 4x more conservative than the watch page's own 5s).
		const std::string nextToken =
			Str(Obj(Obj(resp.body, "continuation"), "timedContinuationData"), "continuation");

		const json &renderer = ViewershipRenderer(resp.body);
		if (!renderer.is_object()) {
			// A bogus id answers 200 carrying only responseContext, and a stale continuation
			// answers 200 with nothing to update -- indistinguishable here, so a RESUMED read
			// spends one retry on the videoId form before giving up on the id.
			if (resumed) {
				DBG(LogCat::OAuth,
				    "youtube viewers: dest=%s resumed read carried no viewership action, "
				    "re-reading from the video id",
				    destTag.c_str());
				SetViewerContinuation(dest, std::string());
				continuation.clear();
				continue;
			}
			DBG(LogCat::OAuth, "youtube viewers: dest=%s no viewership action -> leaving the count ABSENT",
			    destTag.c_str());
			SetViewerContinuation(dest, nextToken);
			return true;
		}
		SetViewerContinuation(dest, nextToken);

		if (!Bool(renderer, "isLive")) {
			DBG(LogCat::OAuth,
			    "youtube viewers: dest=%s renderer is not isLive -> the figure is cumulative views, "
			    "leaving the count ABSENT",
			    destTag.c_str());
			return true;
		}

		// originalViewCount is the raw unlocalized integer ("7175"); unlabeledViewCountValue is
		// the same number as display text ("7,175"). viewCount.simpleText is NEVER read -- it is
		// localized prose ("7,175 watching now") and parsing it is how a locale change silently
		// becomes a wrong number.
		int count = 0;
		if (!ParseViewerCount(Str(renderer, "originalViewCount"), count) &&
		    !ParseViewerCount(Str(Obj(renderer, "unlabeledViewCountValue"), "simpleText"), count)) {
			DBG(LogCat::OAuth,
			    "youtube viewers: dest=%s isLive renderer carried no parseable count -> leaving it ABSENT",
			    destTag.c_str());
			return true;
		}
		// Both figures are logged: the adjusted one is what the app shows, and the reported one
		// is what YouTube actually said. Printing only the adjustment would make a future
		// investigation of this floor start by re-deriving the raw number.
		const int reported = count;
		count = std::max(0, reported - kYouTubeLiveViewerFloor);
		out[videoId] = count;
		DBG(LogCat::OAuth,
		    "youtube viewers: dest=%s isLive -> %d concurrent viewers "
		    "(reported %d, less the platform floor of %d)",
		    destTag.c_str(), count, reported, kYouTubeLiveViewerFloor);
		return true;
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

	// DISTINCT broadcast ids. The dedupe is belt-and-braces against two destinations naming one
	// broadcast (only reachable if two profiles somehow remembered the same ingest stream):
	// reporting it twice would double-count those viewers in the caller's sum. The first
	// destination claiming an id keeps it, matching the previous behavior.
	std::vector<std::pair<DestinationId, std::string>> claims; // one entry per distinct broadcast
	std::set<std::string> seen;
	for (const auto &target : targets) {
		if (!seen.insert(target.second).second) {
			continue; // this broadcast is already represented by an earlier destination
		}
		claims.emplace_back(target.first, target.second);
	}

	// One request per broadcast: InnerTube's updated_metadata reads a single video and has no
	// batch form, and since it bills no quota the per-id shape costs nothing but a round trip.
	// An id that fails leaves ITS entry absent from `counts` and therefore its destination
	// absent from `out`, exactly as a failed batch read did.
	// BRAIDCAST_YOUTUBE_VIEWERS=false stops YouTube's viewer read without touching any other
	// platform's, so a live broadcast can be observed with and without this traffic. It exists
	// because a quiet broadcast reported a floor of exactly 1 rather than 0, and the only way to
	// tell an inflating read apart from a genuine viewer is to remove the read and look again.
	// Leaving `counts` empty keeps every destination absent from `out`, which is already how this
	// path spells "no figure" -- so the UI shows nothing rather than a fabricated zero.
	std::map<std::string, int> counts;
	if (Env::Flag("BRAIDCAST_YOUTUBE_VIEWERS", true)) {
		for (const auto &claim : claims) {
			std::string readErr;
			if (!ReadBroadcastViewers(claim.first, claim.second, counts, readErr) && err.empty()) {
				// One id failing must not discard the ids that did read -- keep the first
				// error for the caller's log and carry on, so a partial total still beats
				// no total.
				err = readErr;
			}
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
	if (!Http::Require2xx(resp, "YouTube channels request", err)) {
		return false;
	}

	const json item = First(ParseJson(resp.body), "items");
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
