#include "youtube_chat.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../events/event_hub.hpp"   // Events::Hub().Ingest for monetization/membership events
#include "../events/event_model.hpp" // Events::NormalizedEvent
#include "../log.hpp"                // DBG / LogCat -- gated path-active logging
#include "util/env_config.hpp"
#include "util/http_client.hpp"
#include "util/json_util.hpp"
#include "util/op_error.hpp"
#include "../oauth/youtube_provider.hpp"
#include "util/time_util.hpp"
#include "third_party_emotes.hpp"
#include "ws_client.hpp"         // CancelableSleep / Backoff
#include "youtube_innertube.hpp" // the zero-quota primary read

namespace Chat {

namespace {

const char *kLiveChatMessagesUrl = "https://www.googleapis.com/youtube/v3/liveChat/messages";

// liveChatMessages.streamList: the push-based read, and the default. Google documents both
// gRPC and HTTP/REST for it; this is the HTTP/JSON-transcoded REST surface, which returns
// 200 chunked, the body an incremental JSON array of normal liveChatMessageListResponse
// objects arriving as messages are posted. Billing is 5 units per CONNECTION rather than
// per message, which is why it is the default. If it 404/400s the transcode is unavailable
// and we fall back to the classic .list poll.
//
// The hold is short: measured against 1,660 live connections, the server terminates every
// batch after about ten seconds regardless of traffic, pushing a final empty page that
// carries the next nextPageToken. So this endpoint is a ~10s long-poll in practice, not a
// broadcast-length stream, and the per-connection billing makes the reconnect cadence
// below a direct quota knob.
const char *kLiveChatStreamUrl = "https://www.googleapis.com/youtube/v3/liveChat/messages/stream";

// liveChatMessages.list omits pollingIntervalMillis on rare responses; fall back
// to a conservative interval and never poll faster than this floor (quota guard).
constexpr long kDefaultPollMs = 5000;
constexpr long kMinPollMs = 1500;

// Dead time between one batch closing and the next connection opening. Every reconnect is
// a separately billed 5-unit request, so this is a quota knob, not a free resume: with the
// server's ~10s hold, one chat costs 5 units per (hold + this floor), and lowering it buys
// chat coverage at a one-for-one quota cost. It never rises off the floor in practice --
// streamList sends no pollingIntervalMillis, so the std::max below always picks this.
constexpr long kStreamReconnectFloorMs = 250;

// Below this, a 2xx cycle that delivered nothing is treated as an instant reject rather than
// a batch boundary. The server's real batches run an order of magnitude longer, so a healthy
// cycle never reaches this test.
constexpr long kStreamMinCycleMs = 2000;

// streamList accepts 200..2000 (default 500). Not a quota lever on this endpoint: the
// server ends a batch on its own time deadline rather than on a full page, so a larger cap
// does not reduce the connection count.
constexpr const char *kStreamMaxResults = "500";

// Consecutive delivering-nothing streamList connections tolerated before handing chat to
// the .list poll for the rest of the session. This tests the TRANSPORT, not chat activity:
// a healthy silent chat still yields one frame per connection, because the server ends
// every batch with an empty page carrying the next nextPageToken. So a run of connections
// that parse no frame at all means bytes are not arriving, and each strike costs only the
// server's ~10s batch deadline (the 90s low-speed watchdog bounds the pathological case
// where the response stalls after its headers).
constexpr int kStreamDeadStrikes = 2;

// What ONE request on either charged chat surface costs. liveChatMessages.streamList bills per
// CONNECTION and liveChatMessages.list per POLL, both 5 units, so a single constant covers
// both and the budget below counts real requests rather than an estimate.
constexpr int kChatUnitCost = 5;

// Shown beside the connected state while chat is being read on a charged surface, so a chat
// that may stop before the broadcast does says so up front rather than only afterwards. No
// action is offered because there is none: the free reader handles unlisted and public alike,
// so reaching this surface means the free read failed, was turned off, or this broadcast's
// privacy could not be resolved, and the allowance is all that is left.
constexpr const char *kChargedReadNote = "chat is on a limited daily allowance and may stop before your broadcast ends";

// Ends the chat of a private broadcast, in place of billing it. Privacy is a property of the
// WHOLE broadcast rather than a blip a retry clears, so the charged read would run at ~1,730
// units/chat-hour against a shared pool for its entire duration -- a standing charge no
// destination may take quietly. Ending says so, and says the one thing the user can act on.
constexpr const char *kPrivateChatUnavailable =
	"YouTube chat is not available on private broadcasts. Set this destination's privacy to "
	"unlisted or public before your next stream to see chat here.";

using JsonUtil::Bool;
using JsonUtil::NumLoose;
using JsonUtil::Obj;
using JsonUtil::ParseJson;
using JsonUtil::Str;
using TimeUtil::Rfc3339ToEpochMs;

// Incremental extractor of complete top-level JSON objects from a byte stream, so a
// streamList response can be parsed one liveChatMessageListResponse at a time as its
// bytes arrive. Framing-agnostic: it scans for balanced top-level `{...}` objects and
// ignores array brackets / commas / whitespace between them, so it works whether the
// transcode emits a JSON array (`[{..},{..}]`) or newline-delimited objects. String- and
// escape-aware so a brace inside a message string never miscounts depth. Ported from the
// streamlist-probe reference (class JsonObjectStream).
class JsonObjectStream {
public:
	// Feed a chunk; append any newly-completed top-level objects (raw JSON text) to `out`.
	void Push(std::string_view chunk, std::vector<std::string> &out)
	{
		for (const char c : chunk) {
			buf_.push_back(c);
			const size_t idx = buf_.size() - 1;
			if (inStr_) {
				if (escaped_) {
					escaped_ = false;
				} else if (c == '\\') {
					escaped_ = true;
				} else if (c == '"') {
					inStr_ = false;
				}
				continue;
			}
			if (c == '"') {
				inStr_ = true;
				continue;
			}
			if (c == '{') {
				if (depth_ == 0) {
					objStart_ = static_cast<long long>(idx);
				}
				++depth_;
			} else if (c == '}') {
				if (depth_ > 0) {
					--depth_;
				}
				if (depth_ == 0 && objStart_ >= 0) {
					out.emplace_back(buf_, static_cast<size_t>(objStart_),
							 idx - static_cast<size_t>(objStart_) + 1);
					// Reset so the buffer never grows unbounded across objects.
					buf_.clear();
					objStart_ = -1;
				}
			}
		}
	}

private:
	std::string buf_;
	int depth_ = 0;
	bool inStr_ = false;
	bool escaped_ = false;
	long long objStart_ = -1;
};

// authorDetails.{isChatOwner,isChatModerator,isChatSponsor} -> normalized badge
// kinds. YouTube ships no badge image URLs on live-chat items, so url is omitted.
json BadgesFor(const json &authorDetails)
{
	json badges = json::array();
	if (Bool(authorDetails, "isChatOwner")) {
		badges.push_back(json{{"kind", "broadcaster"}});
	}
	if (Bool(authorDetails, "isChatModerator")) {
		badges.push_back(json{{"kind", "moderator"}});
	}
	if (Bool(authorDetails, "isChatSponsor")) {
		badges.push_back(json{{"kind", "member"}});
	}
	return badges;
}

// One liveChatMessages item -> the Phase 9 normalized message, or a null json when
// the item carries no displayable text (e.g. a non-text event we skip).
json NormalizeItem(const json &item, const std::string &liveChatId,
		   const std::unordered_map<std::string, std::string> &thirdPartyEmotes)
{
	if (!item.is_object()) {
		return json(nullptr);
	}
	const json &snippet = item.contains("snippet") ? item["snippet"] : json(nullptr);
	const json &author = item.contains("authorDetails") ? item["authorDetails"] : json(nullptr);

	std::string text = Str(snippet, "displayMessage");
	if (text.empty() && snippet.is_object() && snippet.contains("textMessageDetails") &&
	    snippet["textMessageDetails"].is_object()) {
		text = Str(snippet["textMessageDetails"], "messageText");
	}
	if (text.empty()) {
		return json(nullptr); // super-chat/membership events without text -- skip in the MVP
	}

	// YouTube's list response carries no emoji image runs, so the message starts as a
	// single plain-text fragment (emoji arrive inline as unicode in displayMessage);
	// the third-party pass then splits any 7TV/BTTV emote words out of that text.
	json fragments = json::array();
	fragments.push_back(json{{"type", "text"}, {"text", text}});
	fragments = ApplyThirdPartyEmotes(fragments, thirdPartyEmotes);

	// The frame itself comes from the shared assembler, which the InnerTube read also uses --
	// the two schemas share no field, so that seam is the only thing keeping the wire shape
	// identical whichever of them is reading this chat.
	return BuildChatMessage("youtube", liveChatId, Str(item, "id"),
				static_cast<int64_t>(Rfc3339ToEpochMs(Str(snippet, "publishedAt"))),
				Str(author, "displayName"), Str(author, "channelId"), std::string(), BadgesFor(author),
				fragments);
}

// Recognize the monetization/membership live-chat item types and fill `ev` with the
// normalized event. Returns false for plain chat (textMessageEvent) and any unhandled
// type. A Super Chat is shown in BOTH the chat feed and the events feed, so the caller
// runs this IN ADDITION to the normal chat emit -- it never suppresses a chat line.
// Super Chat / Sticker ids are content-derived (Events::YouTubeMoneyEventId), matching
// the REST superChatEvents.list path so the same purchase seen by both surfaces (which
// assign different resource ids) collapses in the store. Membership ids stay keyed on the
// resource id (single-path, no cross-surface duplicate).
bool BuildEventFromChat(const json &item, Events::NormalizedEvent &ev)
{
	if (!item.is_object()) {
		return false;
	}
	const json &snippet = Obj(item, "snippet");
	const std::string type = Str(snippet, "type");
	const std::string itemId = Str(item, "id");
	if (itemId.empty()) {
		return false; // no id -> undedupable
	}
	const json &authorDetails = Obj(item, "authorDetails");
	const std::string actor = Str(authorDetails, "displayName");
	const std::string channelId = Str(authorDetails, "channelId");
	const int64_t ts = static_cast<int64_t>(Rfc3339ToEpochMs(Str(snippet, "publishedAt")));

	ev.platform = "youtube";
	ev.actorName = actor;
	ev.ts = ts;

	if (type == "superChatEvent") {
		const json &d = Obj(snippet, "superChatDetails");
		const int64_t micros = NumLoose(d, "amountMicros");
		ev.type = "superchat";
		// Content-derived id, identical to the REST superChatEvents.list key, so the same
		// purchase seen by both surfaces (which assign different resource ids) collapses in
		// the store. Fall back to the message id when the supporter channel is absent (rare).
		ev.id = channelId.empty() ? ("youtube:superchat:" + itemId)
					  : Events::YouTubeMoneyEventId("superchat", channelId, micros, ts / 1000);
		ev.amount = micros / 10000; // micros -> minor units (cents)
		ev.currency = Str(d, "currency");
		ev.message = Str(d, "userComment");
		return true;
	}
	if (type == "superStickerEvent") {
		const json &d = Obj(snippet, "superStickerDetails");
		const int64_t micros = NumLoose(d, "amountMicros");
		ev.type = "supersticker";
		ev.id = channelId.empty() ? ("youtube:supersticker:" + itemId)
					  : Events::YouTubeMoneyEventId("supersticker", channelId, micros, ts / 1000);
		ev.amount = micros / 10000;
		ev.currency = Str(d, "currency");
		return true;
	}
	if (type == "newSponsorEvent") {
		const json &d = Obj(snippet, "newSponsorDetails");
		ev.type = "member";
		ev.id = "youtube:member:" + itemId;
		ev.tier = Str(d, "memberLevelName");
		return true;
	}
	if (type == "memberMilestoneChatEvent") {
		const json &d = Obj(snippet, "memberMilestoneChatDetails");
		ev.type = "member";
		ev.id = "youtube:member:" + itemId;
		ev.months = static_cast<int>(NumLoose(d, "memberMonth"));
		ev.tier = Str(d, "memberLevelName");
		ev.message = Str(d, "userComment");
		return true;
	}
	return false; // textMessageEvent / unhandled -> chat only
}

// Forward one chat-derived event into the events feed, attributed to the destination whose
// chat carried it -- this is the one event source that knows which BROADCAST a purchase
// arrived on (the REST transport reads channel-wide and can only name the account). Shared by
// every read path so the attribution cannot drift between them.
void IngestChatEvent(const ChatContext &ctx, Events::NormalizedEvent &ev)
{
	ev.accountId = ctx.dest.accountId;
	ev.profileUuid = ctx.dest.profileUuid;
	Events::Hub().Ingest(ev);
}

// Process one liveChatMessageListResponse's items[]: emit each chat line and, in addition,
// forward monetization/membership types into the events feed. Shared by the streamList and
// the .list fallback so the emit semantics (chat-line-then-event, cancel-polled) can't
// drift between the two read paths.
void ProcessChatItems(const ChatContext &ctx, const json &items, const std::string &liveChatId,
		      const std::unordered_map<std::string, std::string> &thirdPartyEmotes,
		      const std::function<bool()> &canceled)
{
	if (!items.is_array()) {
		return;
	}
	for (const json &item : items) {
		if (canceled()) {
			break;
		}
		// Chat first: a plain message emits a chat line; a Super Chat / membership item
		// still emits its chat line (it carries text).
		const json msg = NormalizeItem(item, liveChatId, thirdPartyEmotes);
		if (msg.is_object()) {
			ctx.emit(msg);
		}
		// Then, IN ADDITION, forward monetization/membership types into the events feed.
		// YouTube has no real-time event socket, so this live-chat sink is the only push
		// source for Super Chats/memberships. The store dedupes against backfill/poll.
		Events::NormalizedEvent ev;
		if (BuildEventFromChat(item, ev)) {
			IngestChatEvent(ctx, ev);
		}
	}
}

// The shared per-connection context both read loops run on. References into connect()'s
// frame (the loops never outlive it); `announced` is threaded across a stream->list
// fallback so the connected state is emitted once per connected stretch rather than per
// parsed frame -- see AnnounceOnce and ReportDegraded, which are the only two writers.
struct ChatSession {
	OAuth::YouTubeProvider &owner;
	const ChatContext &ctx;
	OAuth::OAuthAccount &acct;
	std::string liveChatId;
	// The same broadcast's video id, for the InnerTube read. Empty when the provider could
	// not resolve one, which simply skips that read path.
	std::string videoId;
	// The same broadcast's privacy, from the same resolution. Empty when the provider could
	// not resolve one, which reads as "not proven private" -- an unknown privacy keeps today's
	// fall-through to the charged read rather than silently disabling chat. A connect-time
	// snapshot: it answers a known-private destination immediately, while the billed gate
	// re-resolves before spending, since only the fresh value can have changed since.
	std::string privacy;
	const std::unordered_map<std::string, std::string> &thirdPartyEmotes;
	std::function<bool()> canceled;
	std::function<void(bool, const std::string &)> emitState;
	std::function<void()> holdLiveChat;
	Backoff &backoff;
	// OAuth::DestinationKey(ctx.dest), rendered once. Every line these loops write lands in
	// one log that all of an account's chats interleave into, so without this the per-frame
	// and per-connection lines cannot be attributed to a broadcast. Held as a built string so
	// a gated-off DBG still costs nothing.
	std::string destTag;
	// An advisory carried alongside the CONNECTED state (empty for the free read). The chat
	// dock renders it next to the state, which is how "chat works, but it is spending quota"
	// becomes visible without pretending the transport is unhealthy.
	std::string note;
	bool announced = false;
	// Consecutive failed read attempts, counted only for the degraded-report grace below and
	// reset by AnnounceOnce. Deliberately NOT a retry budget -- nothing here gives up on a
	// count; the backoff owns pacing and EndSession owns giving up.
	int failedAttempts = 0;
	// Set by EndSession once a loop has ended for good, so connect()'s clean bookend knows
	// not to overwrite the reason. Never cleared: a session has one ending.
	bool terminal = false;
};

// Confirm the chat once: emit the connected state and mark the live-chat forward as an
// authoritative Super Chat source for this account (so the REST event transport backs off
// while any of the account's chats are being read). Idempotent.
void AnnounceOnce(ChatSession &s)
{
	// Every successful read reaches here -- streamList per parsed frame, the .list poll per
	// cycle, InnerTube per successful poll -- so this is where the degraded-grace streak
	// resets, ahead of the latch check that makes the rest of this a no-op.
	s.failedAttempts = 0;
	if (!s.announced) {
		s.emitState(true, s.note);
		s.announced = true;
		s.holdLiveChat();
	}
}

// AnnounceOnce's counterpart, and the ONLY way to report a transient degradation. Clearing
// `announced` is not optional bookkeeping -- it is what re-arms recovery. The latch exists so
// the connected state is not re-sent on every parsed frame (ctx.emit puts a chat.state frame
// on the wire unconditionally, and streamList parses one about every second); the consequence
// is that a degraded report which leaves the latch set turns AnnounceOnce into a permanent
// no-op. The loop then recovers and keeps polling while the row still claims "Reconnecting"
// for the rest of the broadcast, and because an idle chat emits no log line, nothing ever
// contradicts it.
//
// Every backoff path routes through here for that reason: open-coding emitState(false, ...)
// is what let six of the seven sites drift into exactly that bug. Re-arming is safe --
// AnnounceOnce's other obligation, the live-chat refcount hold, is idempotent
// (LiveChatHold::acquire guards on `held`), so replaying it costs nothing.
//
// A single failed attempt is not worth surfacing at all. Both readers poll continuously, so
// one timeout or one 503 normally recovers on the very next request, and a row that flips to
// Reconnecting for those few seconds reports a fault where nothing was lost. Wait for a second
// consecutive failure instead. The cost is exactly one extra attempt, but the wall-clock cost of
// that attempt is whatever the backoff had already grown to -- up to its 30s cap -- so this trades
// a false Reconnecting on a blip for a later true one on a real outage.
constexpr int kDegradedGraceAttempts = 2;

// Whether a degraded report waits out that grace. Transient is every poll-level failure --
// timeout, 5xx, rate-limit -- which is the usual case. Sustained is an outage already known to
// last, the daily quota stand-down, which has to reach the row at once or the row claims
// Connected through a pause measured in hours.
enum class Degradation { Transient, Sustained };

void ReportDegraded(ChatSession &s, const std::string &why, Degradation kind = Degradation::Transient)
{
	if (kind == Degradation::Transient && ++s.failedAttempts < kDegradedGraceAttempts) {
		DBG(LogCat::Chat, "youtube: dest=%s failed attempt %d of %d, holding the row before reporting (%s)",
		    s.destTag.c_str(), s.failedAttempts, kDegradedGraceAttempts, why.c_str());
		return;
	}
	s.emitState(false, why);
	s.announced = false;
}

// The ONE way either read loop ends for good. It owns every obligation of a terminal exit
// together, so a branch cannot satisfy some and silently drop the rest:
//   - the health surface gets Failed rather than Reconnecting (this loop is not coming back),
//   - the reason reaches the chat pane,
//   - `err` carries the reason to the hub's log,
//   - `s.terminal` stops connect()'s clean bookend from overwriting it with an empty state.
// A terminal row's reason IS its whole content, so an empty one is substituted rather than
// shown -- the bug this replaces was a terminal path whose reason never reached the user.
void EndSession(ChatSession &s, const std::string &reason, std::string &err,
		Transports::TransportHealth::State health = Transports::TransportHealth::State::Failed)
{
	const std::string text = reason.empty() ? std::string("YouTube chat stopped") : reason;
	s.terminal = true;
	err = text;
	EmitChatTerminal(s.ctx, "youtube", text, health);
}

// The user-facing reason for a terminal 403, shared by both read loops (chat disabled, chat
// ended, forbidden). YouTube omits the reason on some 403s, so the status stands in.
std::string TerminalHttpReason(long status, const std::string &reason)
{
	return "YouTube chat error: " + (reason.empty() ? "HTTP " + std::to_string(status) : reason);
}

// Shared terminal handling for a chat whose underlying livestream has gone offline, so the
// reason and the exit contract cannot drift between the two read loops.
void EndOnChatOffline(ChatSession &s, const std::string &offlineAt, const char *via, std::string &err)
{
	const std::string reason =
		std::string("YouTube live chat ended") + (offlineAt.empty() ? std::string() : " at " + offlineAt);
	DBG(LogCat::Chat, "youtube %s: dest=%s offlineAt=%s -> chat ended, stopping reads", via, s.destTag.c_str(),
	    offlineAt.c_str());
	EndSession(s, reason, err);
}

// --- the floor under the charged read ----------------------------------------------------
//
// InnerTube reads chat for free for anything a logged-out viewer may watch -- public AND
// unlisted -- so the charged surface is reached only by a private broadcast, which it cannot
// see at all, or by a free read that failed for some other reason. Measured at ~1,730
// units/chat-hour per destination, four destinations spend the entire 10,000-unit pool that
// every install shares in about 87 minutes. These two bound that: the private case is refused
// outright (it is the whole broadcast's answer, not a transient failure, so paying for it
// would be a standing charge), and what remains shares the provider's one budget so the sum
// across destinations is what is bounded, not each destination separately.

// The ONE test of "the free reader is blind to this broadcast and the charged one may not pay
// for it", shared by the connect-time answer and the billed gate so a second copy of the
// comparison cannot drift. True -> the session has ENDED with the reason on the chat pane.
//
// Refusing is not free, only far cheaper: a session that ends here never takes the live-chat
// refcount hold, so the REST event transport keeps polling superChatEvents.list for this
// destination at roughly 40 units/hour instead of the ~1,730 the charged read would cost. That
// poll reads the whole channel rather than one broadcast, so a private broadcast's Super Chats
// reach the events feed even with its chat ended here.
bool RefuseIfPrivate(ChatSession &s, const std::string &privacy, std::string &err)
{
	if (privacy != OAuth::kPrivacyPrivate) {
		return false;
	}
	// Unavailable rather than Failed: the app is declining to read this broadcast, which is
	// a property of the broadcast and not something that went wrong with the transport.
	EndSession(s, kPrivateChatUnavailable, err, Transports::TransportHealth::State::Unavailable);
	return true;
}

// May this session move onto a charged read at all? False -> the session has ENDED with the
// reason on the chat pane. Also arms the advisory the connected state carries, so a chat that
// is spending says so before it stops rather than only afterwards.
bool EnterBilledRead(ChatSession &s, std::string &err)
{
	// Re-resolved rather than taken from the session, because this is the moment the answer
	// starts costing money and the connect-time snapshot is the one input that can have gone
	// stale: an edit that flips this broadcast to private mid-stream refreshes the provider's
	// cache, but this transport gets one connect() per go-live and never re-reads it otherwise.
	// A cache hit costs no request. An unresolvable lookup falls back to the snapshot instead of
	// reading as unknown, so a transient miss cannot open the gate on a known-private broadcast.
	const std::string fresh = s.owner.chatBroadcastRef(s.acct, s.ctx.dest.profileUuid).privacy;

	// Before the budget, because this is the more specific answer: a private broadcast is not a
	// chat that ran out of allowance, it is one this app will not read at all. An UNKNOWN
	// privacy is deliberately not caught here -- it falls through to the charged read, since
	// disabling chat on a guess is the worse error.
	if (RefuseIfPrivate(s, fresh.empty() ? s.privacy : fresh, err)) {
		return false;
	}
	if (s.owner.ChatBudgetExhausted()) {
		EndSession(s, s.owner.ChatBudgetMessage(), err);
		return false;
	}
	if (s.note.empty()) {
		s.note = kChargedReadNote;
		// Re-confirm so the advisory reaches a pane that was already told "connected" by a
		// free read that has since handed over. AnnounceOnce's other obligation (the
		// live-chat refcount hold) is idempotent, so replaying it costs nothing.
		s.announced = false;
		HostLog("[chat] youtube: dest=" + s.destTag +
			" is reading chat on YouTube's quota-billed API; it will stop when this install's "
			"daily chat budget is spent. The free reader handles unlisted and public alike, so "
			"this destination's free read failed, was turned off, or its privacy could not be "
			"resolved.");
	}
	return true;
}

// Reserve one charged request. False -> the budget ran out mid-session and the session has
// ENDED with the reason on the chat pane; the caller must not send.
bool ChargeChatRequest(ChatSession &s, std::string &err)
{
	if (s.owner.ChargeChatUnits(kChatUnitCost)) {
		return true;
	}
	EndSession(s, s.owner.ChatBudgetMessage(), err);
	return false;
}

// Read this destination's chat over InnerTube -- the PRIMARY read, because it costs zero
// quota. Purely glue: the protocol lives in youtube_innertube, and the callbacks below route
// its output through the SAME session obligations the Data API loops use (AnnounceOnce for the
// connected state plus the live-chat refcount hold, EndSession for a terminal exit,
// IngestChatEvent for the attribution), so the two read paths cannot diverge on any of them.
// Returns true to request the official read; false when the session ended or was canceled.
bool RunInnerTube(ChatSession &s, std::string &err)
{
	YouTubeInnerTube::Config cfg;
	cfg.videoId = s.videoId;
	cfg.channelId = s.liveChatId;
	cfg.thirdPartyEmotes = &s.thirdPartyEmotes;
	cfg.destTag = s.destTag;

	YouTubeInnerTube::Callbacks cb;
	cb.canceled = s.canceled;
	cb.emitMessage = [&s](const json &message) {
		s.ctx.emit(message);
	};
	cb.emitEvent = [&s](Events::NormalizedEvent &ev) {
		IngestChatEvent(s.ctx, ev);
	};
	// Reusing AnnounceOnce is what keeps this destination's live-chat refcount held for an
	// InnerTube read exactly as it is for a Data API read. Without that hold
	// ShouldPollSuperChats concludes the broadcast's chat is uncovered and superChatEvents.list
	// resumes spending -- reintroducing the very cost this read path exists to remove.
	cb.announce = [&s] {
		AnnounceOnce(s);
	};
	cb.terminal = [&s, &err](const std::string &reason) {
		EndSession(s, reason, err);
	};
	// Degraded reports go through ReportDegraded rather than emitState so the announce latch
	// re-arms: the InnerTube loop has no connected-state call of its own, it only calls
	// cb.announce on a successful poll, and AnnounceOnce is a no-op while the latch is set.
	// The connected arm goes through AnnounceOnce rather than emitState directly so it cannot
	// satisfy half of the announce contract: no InnerTube path calls state(true) today, and if
	// one is added it must take the live-chat refcount hold like every other connected report,
	// or ShouldPollSuperChats resumes billing against a chat this read already covers.
	cb.state = [&s](bool connected, const std::string &stateErr) {
		if (connected) {
			AnnounceOnce(s);
			return;
		}
		ReportDegraded(s, stateErr);
	};
	return YouTubeInnerTube::Run(cfg, cb);
}

// While the shared daily-quota gate is closed, surface the outage on the chat state
// and sleep (cancelable) until the reset instant, so neither read loop spends a
// request against a spent quota. Returns true when the wait was canceled.
bool WaitOutQuotaExhaustion(ChatSession &s)
{
	std::chrono::milliseconds wait{};
	if (!s.owner.QuotaExhausted(&wait)) {
		return false;
	}
	DBG(LogCat::Chat, "youtube: dest=%s quota exhausted, chat paused %lldms until the reset", s.destTag.c_str(),
	    static_cast<long long>(wait.count()));
	// The provider's own sentence for this outage, not a second wording of it: the same
	// stand-down also refuses go-live and every other YouTube request, and two descriptions of
	// one outage drifted apart once already.
	ReportDegraded(s, s.owner.QuotaMessage(), Degradation::Sustained);
	return CancelableSleep(wait, s.canceled);
}

// Drive the push-based streamList read loop. Returns true to request the .list fallback
// (the transcode endpoint is unavailable: HTTP 404/400, or the stream connects but never
// delivers -- see kStreamDeadStrikes); false when the session ended for any other reason
// (cancel, terminal error, or an unrecoverable re-auth with `err` set).
bool RunStreamList(ChatSession &s, std::string &err)
{
	std::string pageToken;
	// The first response object on a COLD connect is backlog; suppress it so the user sees
	// messages from connect onward. This is set false after that first object and never
	// again, so reconnects (which resume from a nextPageToken) emit normally.
	bool firstConnect = true;
	// Consecutive 2xx connections that parsed no response object at all. A healthy stream
	// yields at least one object per connection even on a silent chat -- the server ends
	// each batch with a response (possibly zero items) carrying a nextPageToken -- so a run
	// of empty connections means this transport is not delivering on this machine. Reset by
	// any connection that parses a frame. Counts 2xx responses only: a status 0 is a
	// connect/TLS failure with no response at all, which the backoff-and-retry path already
	// owns and which says nothing about whether the endpoint delivers once reached.
	int deadStreak = 0;
	// Consecutive cycles held off by the kStreamMinCycleMs guard below. Only its leading edge
	// is host-logged, so a sustained reject loop stays visible without flooding the log.
	int fastEmptyStreak = 0;

	while (!s.canceled()) {
		if (WaitOutQuotaExhaustion(s)) {
			break;
		}
		// Before the URL is even built: the budget is what decides whether this connection
		// happens, and a refusal ends the session rather than falling through to .list,
		// which bills the same units against the same broadcast.
		if (!ChargeChatRequest(s, err)) {
			return false;
		}
		std::string url = std::string(kLiveChatStreamUrl) + "?liveChatId=" + Http::UrlEncode(s.liveChatId) +
				  "&part=id,snippet,authorDetails&maxResults=" + kStreamMaxResults;
		if (!pageToken.empty()) {
			url += "&pageToken=" + Http::UrlEncode(pageToken);
		}

		Http::HttpReq req;
		req.method = "GET";
		req.url = url;

		JsonObjectStream objects;
		long serverPollMs = 0; // server pollingIntervalMillis advised on this connection
		int frameCount = 0;    // complete response objects parsed on this connection
		long bytesIn = 0;      // decoded stream bytes fed to the parser on this connection
		long itemsIn = 0;      // live-chat items carried by those objects, backlog included
		std::string offlineAt; // set once a frame reports the livestream offline

		// Parse + process each complete response object the moment its bytes finish
		// arriving, so a live push shows up with ~1s latency instead of waiting for the
		// batch to end. Runs on this worker thread inside curl's write callback.
		auto onChunk = [&](std::string_view chunk) -> bool {
			if (s.canceled()) {
				return false;
			}
			bytesIn += static_cast<long>(chunk.size());
			std::vector<std::string> ready;
			objects.Push(chunk, ready);
			for (const std::string &objText : ready) {
				const json resp = ParseJson(objText);
				if (!resp.is_object()) {
					DBG(LogCat::Chat,
					    "youtube streamList: dest=%s skipped unparseable frame (%ld bytes)",
					    s.destTag.c_str(), static_cast<long>(objText.size()));
					continue;
				}
				++frameCount;
				AnnounceOnce(s);
				const json &items = Obj(resp, "items");
				const int n = items.is_array() ? static_cast<int>(items.size()) : 0;
				itemsIn += n;
				if (firstConnect) {
					firstConnect = false;
					DBG(LogCat::Chat,
					    "youtube streamList: dest=%s connect frame items=%d (suppressed "
					    "as backlog)",
					    s.destTag.c_str(), n);
				} else {
					DBG(LogCat::Chat, "youtube streamList: dest=%s frame items=%d -> emitting",
					    s.destTag.c_str(), n);
					ProcessChatItems(s.ctx, items, s.liveChatId, s.thirdPartyEmotes, s.canceled);
				}
				const std::string next = Str(resp, "nextPageToken");
				if (!next.empty()) {
					pageToken = next;
				}
				auto it = resp.find("pollingIntervalMillis");
				if (it != resp.end() && it->is_number()) {
					serverPollMs = it->get<long>();
				}
				// Stop reading as soon as the stream is reported offline: this frame's items
				// were just emitted and nothing further can arrive on an ended chat, so hold
				// the connection no longer. The loop below turns this into a terminal exit.
				offlineAt = Str(resp, "offlineAt");
				if (!offlineAt.empty()) {
					return false;
				}
				if (s.canceled()) {
					return false;
				}
			}
			return true;
		};

		std::string errorBody;
		std::string reqErr;
		const auto cycleStart = std::chrono::steady_clock::now();
		const long status = s.owner.SendAuthedStreaming(s.acct, req, onChunk, errorBody, reqErr);
		// The quota preflight may hand back a {diagnostic, user message} envelope;
		// unpack before this loop logs or emitState()s the string.
		reqErr = Err::Diagnostic(reqErr);

		// `reqErr` on a 2xx distinguishes a clean end-of-response from a post-headers abort
		// (low-speed watchdog, TLS reset, HTTP/2 GOAWAY), which reach here as status 200 and
		// are otherwise indistinguishable from a normal batch close. Logged because that
		// difference decides whether the reconnect cadence below is server policy or a local
		// middlebox truncating the stream.
		DBG(LogCat::Chat,
		    "youtube streamList: dest=%s connection ended status=%ld frames=%d bytes=%ld pollAdvised=%ldms "
		    "err=%s",
		    s.destTag.c_str(), status, frameCount, bytesIn, serverPollMs,
		    reqErr.empty() ? "none" : reqErr.c_str());

		if (s.canceled()) {
			break;
		}
		// Ahead of every other outcome, including the dead-strike counter and the
		// minimum-cycle guard. An ended chat answers 200 with an empty items[] and would
		// otherwise look to the guard like a fast empty cycle and sit in its backoff forever,
		// or trip the dead strikes and hand over to .list -- which bills the same units
		// against the same dead chat. The only correct response is to stop reading it.
		if (!offlineAt.empty()) {
			EndOnChatOffline(s, offlineAt, "streamList", err);
			return false;
		}
		if (status == 0) {
			// Transport failure: transient blip, back off and reconnect.
			DBG(LogCat::Chat, "youtube streamList: dest=%s transport failure (%s), backing off",
			    s.destTag.c_str(), reqErr.c_str());
			ReportDegraded(s, reqErr);
			if (CancelableSleep(s.backoff.next(), s.canceled)) {
				break;
			}
			continue;
		}
		if (status == 404 || status == 400) {
			DBG(LogCat::Chat, "youtube: dest=%s streamList unavailable (HTTP %ld), falling back to list",
			    s.destTag.c_str(), status);
			return true;
		}
		if (status == 401) {
			// Unrecoverable after a forced refresh: re-auth needed. Terminal.
			DBG(LogCat::Chat, "youtube streamList: dest=%s terminal HTTP 401 (%s), ending session",
			    s.destTag.c_str(), reqErr.c_str());
			EndSession(s, reqErr, err);
			return false;
		}
		if (status == 403 || status == 429) {
			const std::string reason = status == 403 ? OAuth::YouTubeErrorReason(errorBody) : std::string();
			const OAuth::YouTubeErrorClass cls = OAuth::ClassifyYouTubeError(status, reason);
			if (cls == OAuth::YouTubeErrorClass::QuotaExhausted) {
				// SendAuthedStreaming already armed the shared gate; the loop-top wait
				// reports the outage and sleeps until the reset.
				DBG(LogCat::Chat, "youtube streamList: dest=%s HTTP %ld (%s) -> quota exhausted",
				    s.destTag.c_str(), status, reason.c_str());
				continue;
			}
			if (cls == OAuth::YouTubeErrorClass::RateLimited) {
				DBG(LogCat::Chat, "youtube streamList: dest=%s HTTP %ld (%s) rate-limited, backing off",
				    s.destTag.c_str(), status, reason.c_str());
				ReportDegraded(s, "YouTube chat rate-limited, retrying");
				if (CancelableSleep(s.backoff.next(), s.canceled)) {
					break;
				}
				continue;
			}
			// `reason` is parsed only for 403, so any other terminal status would
			// otherwise end the session naming neither a cause nor the payload.
			DBG(LogCat::Chat,
			    "youtube streamList: dest=%s terminal HTTP %ld (%s), ending session (body=%s)",
			    s.destTag.c_str(), status, reason.c_str(), Http::BodyForLog(errorBody).c_str());
			EndSession(s, TerminalHttpReason(status, reason), err);
			break;
		}
		if (status < 200 || status >= 300) {
			DBG(LogCat::Chat, "youtube streamList: dest=%s HTTP %ld, backing off", s.destTag.c_str(),
			    status);
			ReportDegraded(s, "HTTP " + std::to_string(status));
			if (CancelableSleep(s.backoff.next(), s.canceled)) {
				break;
			}
			continue;
		}

		// 2xx: the server pushed its batch then closed the connection (cleanly, or a
		// mid-batch drop -- reqErr may be set, but every object that arrived was already
		// processed).
		const long cycleMs = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
							       std::chrono::steady_clock::now() - cycleStart)
							       .count());

		// A 2xx that yielded nothing parseable is the silent-failure shape: hand chat to
		// .list rather than reconnecting into the same void for the rest of the broadcast.
		deadStreak = frameCount > 0 ? 0 : deadStreak + 1;
		if (deadStreak >= kStreamDeadStrikes) {
			HostLog("[chat] youtube: dest=" + s.destTag +
				" streamList connected but delivered nothing "
				"across " +
				std::to_string(deadStreak) + " connections; falling back to liveChatMessages.list");
			return true;
		}

		// An instant 2xx that carried no content is a reject, not a batch boundary -- a stale
		// pageToken or an edge refusing the transcode answers immediately and still ships one
		// parseable terminal page, so the frame-counting deadStreak above cannot see it.
		// Resuming such a cycle on kStreamReconnectFloorMs spins at four billed requests per
		// second per chat, which spends the whole 10,000-unit day inside two minutes, so this
		// takes the shared exponential backoff (capped at 30s by the Backoff the caller
		// builds) and lets it grow while the condition holds. The test is ELAPSED TIME plus
		// "carried nothing" -- deliberately neither of the two predicates already here, since
		// a short batch that did carry messages is healthy and must not be throttled.
		if (cycleMs < kStreamMinCycleMs && frameCount <= 1 && itemsIn == 0) {
			const std::chrono::milliseconds wait = s.backoff.next();
			if (++fastEmptyStreak == 1) {
				HostLog("[chat] youtube: dest=" + s.destTag +
					" streamList returned an empty batch in " + std::to_string(cycleMs) +
					"ms; backing off " + std::to_string(static_cast<long long>(wait.count())) +
					"ms to protect the daily quota");
			} else {
				DBG(LogCat::Chat,
				    "youtube streamList: dest=%s empty %ldms cycle #%d, backing off %lldms",
				    s.destTag.c_str(), cycleMs, fastEmptyStreak, static_cast<long long>(wait.count()));
			}
			if (CancelableSleep(wait, s.canceled)) {
				break;
			}
			continue;
		}
		fastEmptyStreak = 0;
		s.backoff.reset();

		// Resume from the last nextPageToken so the server does not rebuild historical state;
		// honor a larger advisory pollingIntervalMillis if one arrived.
		const long waitMs = std::max<long>(kStreamReconnectFloorMs, serverPollMs);
		DBG(LogCat::Chat, "youtube streamList: dest=%s batch closed after %ldms, resuming in %ldms (token=%s)",
		    s.destTag.c_str(), cycleMs, waitMs, pageToken.empty() ? "none" : "set");
		if (CancelableSleep(std::chrono::milliseconds(waitMs), s.canceled)) {
			break;
		}
	}

	DBG(LogCat::Chat, "youtube streamList: dest=%s read loop exited (canceled=%d)", s.destTag.c_str(),
	    s.canceled() ? 1 : 0);
	return false;
}

// Drive the classic liveChatMessages.list poll loop: the documented fallback used when the
// streamList transcode endpoint is unavailable. Honors the server-dictated
// pollingIntervalMillis + nextPageToken cursor and the same quota-error classification.
void RunListPoll(ChatSession &s, std::string &err)
{
	std::string pageToken;
	bool firstPoll = true;

	while (!s.canceled()) {
		if (WaitOutQuotaExhaustion(s)) {
			break;
		}
		if (!ChargeChatRequest(s, err)) {
			return;
		}
		std::string url = std::string(kLiveChatMessagesUrl) + "?liveChatId=" + Http::UrlEncode(s.liveChatId) +
				  "&part=snippet,authorDetails";
		if (!pageToken.empty()) {
			url += "&pageToken=" + Http::UrlEncode(pageToken);
		}

		Http::HttpReq req;
		req.method = "GET";
		req.url = url;

		Http::HttpResponse resp;
		std::string reqErr;
		if (!s.owner.SendAuthed(s.acct, req, resp, reqErr)) {
			// Quota-preflight refusals arrive as a {diagnostic, user message}
			// envelope; unpack before logging or emitState()ing the string.
			reqErr = Err::Diagnostic(reqErr);
			// SendAuthed fails only on a transport error (status 0) or an
			// unrecoverable 401. The former is a transient blip worth a backoff
			// retry; the latter is fatal -- re-auth is needed.
			if (resp.status == 0) {
				DBG(LogCat::Chat, "youtube list: dest=%s transport failure (%s), backing off",
				    s.destTag.c_str(), reqErr.c_str());
				ReportDegraded(s, reqErr);
				if (CancelableSleep(s.backoff.next(), s.canceled)) {
					break;
				}
				continue;
			}
			DBG(LogCat::Chat, "youtube list: dest=%s terminal HTTP %ld (%s), ending session",
			    s.destTag.c_str(), resp.status, reqErr.c_str());
			EndSession(s, reqErr, err);
			return;
		}

		// A 429, or a 403 whose reason is rate-limit class, is transient: a visible
		// reason, a backoff, and a retry. A quota-exhausted 403 stands down until the
		// midnight-Pacific reset via the shared gate. Any other 403 (chat disabled/
		// ended/forbidden) and every 404 (broadcast gone) end the session.
		if (resp.status == 403 || resp.status == 429) {
			const std::string reason = resp.status == 403 ? OAuth::YouTubeErrorReason(resp.body)
								      : std::string();
			const OAuth::YouTubeErrorClass cls = OAuth::ClassifyYouTubeError(resp.status, reason);
			if (cls == OAuth::YouTubeErrorClass::QuotaExhausted) {
				// SendAuthed already armed the shared gate; the loop-top wait reports
				// the outage and sleeps until the reset.
				DBG(LogCat::Chat, "youtube list: dest=%s HTTP %ld (%s) -> quota exhausted",
				    s.destTag.c_str(), resp.status, reason.c_str());
				continue;
			}
			if (cls == OAuth::YouTubeErrorClass::RateLimited) {
				DBG(LogCat::Chat, "youtube list: dest=%s HTTP %ld (%s) rate-limited, backing off",
				    s.destTag.c_str(), resp.status, reason.c_str());
				ReportDegraded(s, "YouTube chat rate-limited, retrying");
				if (CancelableSleep(s.backoff.next(), s.canceled)) {
					break;
				}
				continue;
			}
			DBG(LogCat::Chat, "youtube list: dest=%s terminal HTTP %ld (%s), ending session",
			    s.destTag.c_str(), resp.status, reason.c_str());
			EndSession(s, TerminalHttpReason(resp.status, reason), err);
			break;
		}
		if (resp.status == 404) {
			DBG(LogCat::Chat, "youtube list: dest=%s HTTP 404 (broadcast gone), ending session",
			    s.destTag.c_str());
			EndSession(s, "YouTube broadcast is no longer available", err);
			break;
		}
		if (resp.status < 200 || resp.status >= 300) {
			DBG(LogCat::Chat, "youtube list: dest=%s HTTP %ld, backing off", s.destTag.c_str(),
			    resp.status);
			ReportDegraded(s, "HTTP " + std::to_string(resp.status));
			if (CancelableSleep(s.backoff.next(), s.canceled)) {
				break;
			}
			continue;
		}

		s.backoff.reset();
		const json j = ParseJson(resp.body);
		pageToken = Str(j, "nextPageToken");

		long pollMs = kDefaultPollMs;
		if (j.is_object()) {
			auto it = j.find("pollingIntervalMillis");
			if (it != j.end() && it->is_number()) {
				pollMs = it->get<long>();
			}
		}
		if (pollMs < kMinPollMs) {
			pollMs = kMinPollMs;
		}

		AnnounceOnce(s);

		// First response is backlog: keep only the cursor, emit nothing, so the user sees
		// messages from connect onward rather than a wall of history.
		const json &items = Obj(j, "items");
		const int n = items.is_array() ? static_cast<int>(items.size()) : 0;
		if (firstPoll) {
			firstPoll = false;
			DBG(LogCat::Chat, "youtube list: dest=%s first poll items=%d (suppressed as backlog)",
			    s.destTag.c_str(), n);
		} else {
			DBG(LogCat::Chat, "youtube list: dest=%s poll items=%d -> emitting", s.destTag.c_str(), n);
			ProcessChatItems(s.ctx, items, s.liveChatId, s.thirdPartyEmotes, s.canceled);
		}

		// Same exposure as the streamList loop: an ended chat keeps answering 200 forever, and
		// none of the error branches above can see it. Checked after this poll's items are
		// emitted so a final batch is not dropped.
		const std::string offlineAt = Str(j, "offlineAt");
		if (!offlineAt.empty()) {
			EndOnChatOffline(s, offlineAt, "list", err);
			return;
		}

		if (CancelableSleep(std::chrono::milliseconds(pollMs), s.canceled)) {
			break;
		}
	}

	DBG(LogCat::Chat, "youtube list: dest=%s poll loop exited (canceled=%d)", s.destTag.c_str(),
	    s.canceled() ? 1 : 0);
}

// The terminal fallback: .list has nothing to hand over to, so it always reports "done".
bool RunListPollFinal(ChatSession &s, std::string &err)
{
	RunListPoll(s, err);
	return false;
}

// The read paths in priority order. Each may hand over to the NEXT one when its endpoint is
// unavailable or stops delivering; the last one never hands over, so chat always lands
// somewhere. Adding or reordering a read is a row here rather than a new branch in connect().
//
// InnerTube is FIRST because it costs ZERO quota. Both Data API reads bill against ONE Cloud
// project's 10,000-unit daily budget shared by every install, which a single user streaming
// continuously to a few destinations exhausts many times over -- so they belong behind a read
// that does not, kept as the fallback for what InnerTube cannot do (it is not authoritative on
// why a chat ended, and it cannot see member-only chat).
//
// `env` forces its path to be skipped, for diagnosing one read against another:
// BRAIDCAST_YOUTUBE_INNERTUBE=false drops to the Data API entirely, and
// BRAIDCAST_YOUTUBE_STREAMLIST=false additionally forces the .list poll.
struct ReadPath {
	const char *env;
	const char *label;
	bool (*run)(ChatSession &, std::string &);
	// Bills Data API quota per request. A billed path is entered only while the daily chat
	// budget has room, and every request it makes is charged against it. A column rather
	// than a name test so adding a read declares its own cost.
	bool billed;
};

const ReadPath kReadPaths[] = {
	{"BRAIDCAST_YOUTUBE_INNERTUBE", "InnerTube live_chat", RunInnerTube, false},
	{"BRAIDCAST_YOUTUBE_STREAMLIST", "liveChatMessages.streamList", RunStreamList, true},
	{nullptr, "liveChatMessages.list", RunListPollFinal, true},
};

} // namespace

bool YouTubeChat::connect(const ChatContext &ctx, OAuth::OAuthAccount &acct, const std::string &channelRef,
			  std::string &err)
{
	// Serialize against an overlapping re-Start: the hub does not join old workers, so a
	// prior connect() on THIS transport might still be unwinding.
	std::lock_guard<std::mutex> run(runMutex_);

	// No active broadcast -> nothing to poll, and nothing here will start one: this transport
	// gets one connect() per go-live. Terminal rather than silent, or the hub's Connecting
	// bookend is the last thing written to this destination's health row and sits there for
	// the rest of the stream. `err` stays empty so the hub still logs nothing.
	if (channelRef.empty()) {
		err.clear();
		EmitChatTerminal(ctx, "youtube", "No active YouTube broadcast to read chat from",
				 Transports::TransportHealth::State::Unavailable);
		return false;
	}
	const std::string liveChatId = channelRef;
	{
		const std::lock_guard<std::mutex> guard(targetMutex_);
		liveChatId_ = liveChatId;
	}
	stop_.store(false, std::memory_order_release);

	// The video id the InnerTube read polls plus that broadcast's privacy (which decides
	// whether the charged read may be entered at all), resolved through the SAME provider seam
	// that produced this liveChatId (a broadcast's id IS its video id), including that seam's
	// cache-miss recovery. One resolution for both, so they can never end up describing
	// different broadcasts, and on the normal cache-hit path -- which this is, since a
	// liveChatId already resolved -- it costs no request at all.
	const OAuth::ChatBroadcastRef broadcast = owner_.chatBroadcastRef(acct, ctx.dest.profileUuid);

	// Release this loop's hold on THIS DESTINATION's live-chat refcount on EVERY exit from
	// this function -- the normal post-loop return, a reconnect give-up, and the
	// unrecoverable 401 that returns from inside the loop -- so the REST event transport
	// resumes covering this broadcast's Super Chats the moment its chat stops being read.
	// Acquired below only once a poll succeeds, and `held` keeps the pair balanced whether
	// or not that happened.
	struct LiveChatHold {
		OAuth::YouTubeProvider &p;
		OAuth::DestinationId dest;
		bool held = false;

		void acquire()
		{
			if (!held) {
				p.AddLiveChatRef(dest);
				held = true;
			}
		}
		~LiveChatHold()
		{
			if (held) {
				p.ReleaseLiveChatRef(dest);
			}
		}
	} liveChatHold{owner_, ctx.dest};

	// Stop advertising a send target once this loop is done, so a send racing teardown
	// fails cleanly instead of posting into a broadcast nobody is reading.
	struct TargetGuard {
		YouTubeChat &self;
		~TargetGuard()
		{
			const std::lock_guard<std::mutex> guard(self.targetMutex_);
			self.liveChatId_.clear();
		}
	} targetGuard{*this};

	auto canceled = [&] {
		return stop_.load(std::memory_order_acquire) || (ctx.canceled && ctx.canceled());
	};
	auto emitState = [&](bool connected, const std::string &stateErr) {
		EmitChatState(ctx, "youtube", connected, stateErr);
	};

	// Build the third-party (7TV/BTTV) emote map once for this session, keyed by the
	// broadcaster's UC channel id (acct.userId). Best-effort + cancel-polled; runs on
	// this worker before the poll loop. Only READ below on this same thread.
	const std::unordered_map<std::string, std::string> thirdPartyEmotes =
		FetchThirdPartyEmotes(EmotePlatform::YouTube, "", acct.userId, canceled);

	auto holdLiveChat = [&liveChatHold] {
		liveChatHold.acquire();
	};

	Backoff backoff(std::chrono::milliseconds(2000), std::chrono::milliseconds(30000));
	ChatSession session{owner_,
			    ctx,
			    acct,
			    liveChatId,
			    broadcast.videoId,
			    broadcast.privacy,
			    thirdPartyEmotes,
			    canceled,
			    emitState,
			    holdLiveChat,
			    backoff,
			    OAuth::DestinationKey(ctx.dest)};

	// Tail only: the full liveChatId is a broadcast-scoped identifier and this log is something
	// users paste into issue reports. Enough to tie this session's dest tag to the hub's
	// connect line without reproducing the id.
	const std::string chatIdTail = liveChatId.size() > 8 ? liveChatId.substr(liveChatId.size() - 8) : liveChatId;

	// A destination already known private at connect time has no readable path at all -- the
	// free read cannot see it and the billed gate below refuses it -- so answer now instead of
	// after the free read has spent its bootstrap retries on a video it can never fetch. Latency
	// only: the gate stays the authoritative test, since it re-resolves a privacy that may have
	// changed after this point.
	if (!canceled() && RefuseIfPrivate(session, session.privacy, err)) {
		return false;
	}

	// Walk the ladder: run the first enabled read, and follow it to the next one only while a
	// read asks to hand over. Every path shares ONE session, so the connected state, the
	// live-chat refcount hold and a terminal reason survive a handover exactly once (both
	// AnnounceOnce and the hold's acquire are idempotent) -- one logical reader per
	// destination at a time, never two.
	for (const ReadPath &path : kReadPaths) {
		if (path.env && !Env::Flag(path.env, true)) {
			DBG(LogCat::Chat, "youtube: dest=%s %s disabled by %s", session.destTag.c_str(), path.label,
			    path.env);
			continue;
		}
		if (canceled()) {
			break;
		}
		if (path.billed && !EnterBilledRead(session, err)) {
			break;
		}
		DBG(LogCat::Chat, "youtube: dest=%s chat=..%s opening via %s", session.destTag.c_str(),
		    chatIdTail.c_str(), path.label);
		backoff.reset();
		if (!path.run(session, err)) {
			break;
		}
	}

	// The clean (false, "") bookend belongs only to an exit that reported nothing final -- a
	// cancel, or a give-up after retries. A loop that ended for good already reported its
	// terminal state and reason through EndSession, and an empty state after that would
	// overwrite the one thing the user needs. The flag gates this rather than `err` so a
	// future terminal branch cannot reintroduce the clobber by forgetting to set `err`;
	// EndSession sets both together.
	if (!session.terminal) {
		emitState(false, "");
	}
	return false;
}

bool YouTubeChat::send(OAuth::OAuthAccount &acct, const std::string &text, std::string &err)
{
	// This transport's own read target, not a fresh provider lookup: the provider holds one
	// broadcast per destination, and re-resolving here would post into whichever of the
	// account's broadcasts applied last rather than the one this chat pane is showing.
	std::string liveChatId;
	{
		const std::lock_guard<std::mutex> guard(targetMutex_);
		liveChatId = liveChatId_;
	}
	if (liveChatId.empty()) {
		err = "no active YouTube broadcast";
		return false;
	}

	json body = json{
		{"snippet", json{{"liveChatId", liveChatId},
				 {"type", "textMessageEvent"},
				 {"textMessageDetails", json{{"messageText", text}}}}},
	};

	Http::HttpReq req;
	req.method = "POST";
	req.url = std::string(kLiveChatMessagesUrl) + "?part=snippet";
	req.contentType = "application/json";
	req.body = body.dump();

	Http::HttpResponse resp;
	if (!owner_.SendAuthed(acct, req, resp, err)) {
		return false;
	}
	if (!Http::Require2xx(resp, "YouTube chat send", err)) {
		return false;
	}
	return true;
}

} // namespace Chat
