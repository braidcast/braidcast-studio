#include "youtube_chat.hpp"
#include "../event_names.hpp"

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
#include "../log.hpp"                 // DBG / LogCat -- gated path-active logging
#include "util/http_client.hpp"
#include "util/json_util.hpp"
#include "../oauth/youtube_provider.hpp"
#include "util/time_util.hpp"
#include "third_party_emotes.hpp"
#include "ws_client.hpp" // CancelableSleep / Backoff

namespace Chat {

namespace {

const char *kLiveChatMessagesUrl = "https://www.googleapis.com/youtube/v3/liveChat/messages";

// liveChatMessages.streamList: the push-based read (opt-in via BRAIDCAST_YOUTUBE_STREAMLIST
// while validated live). Google documents gRPC as streamList's primary interface; this is
// the HTTP/JSON-transcoded REST surface. It returns 200 chunked, the body an incremental
// JSON array of normal liveChatMessageListResponse objects arriving as messages are posted.
// If it 404/400s the transcode is unavailable and we fall back to the classic .list poll.
const char *kLiveChatStreamUrl = "https://www.googleapis.com/youtube/v3/liveChat/messages/stream";

// liveChatMessages.list omits pollingIntervalMillis on rare responses; fall back
// to a conservative interval and never poll faster than this floor (quota guard).
constexpr long kDefaultPollMs = 5000;
constexpr long kMinPollMs = 1500;

// After the streamList server cleanly closes a batch, resume from the last nextPageToken
// promptly -- this is a reconnect on a push stream, not a quota-metered poll, so it floors
// far below kMinPollMs (raised only if the server advises a larger pollingIntervalMillis).
constexpr long kStreamReconnectFloorMs = 250;

// streamList accepts 200..2000 (default 500); larger batches mean fewer reconnects.
constexpr const char *kStreamMaxResults = "500";

using JsonUtil::Bool;
using JsonUtil::NumLoose;
using JsonUtil::Obj;
using JsonUtil::ParseJson;
using JsonUtil::Str;
using TimeUtil::Rfc3339ToEpochMs;

// Opt-in env flag: true only when the named var is set to a recognized truthy value.
// Used to gate the still-under-validation streamList read path off by default.
bool EnvFlag(const char *name)
{
	const char *v = getenv(name);
	if (!v) {
		return false;
	}
	const std::string s(v);
	return s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "on";
}

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

	return json{
		{"event", EventNames::kChatMessage},
		{"platform", "youtube"},
		{"channelId", liveChatId},
		{"id", Str(item, "id")},
		{"ts", Rfc3339ToEpochMs(Str(snippet, "publishedAt"))},
		{"author", json{{"name", Str(author, "displayName")}, {"color", ""}, {"badges", BadgesFor(author)}}},
		{"fragments", fragments},
	};
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
			Events::Hub().Ingest(ev);
		}
	}
}

// The shared per-connection context both read loops run on. References into connect()'s
// frame (the loops never outlive it); `announced` is threaded across a stream->list
// fallback so the connected state is emitted exactly once.
struct ChatSession {
	OAuth::YouTubeProvider &owner;
	const ChatContext &ctx;
	OAuth::OAuthAccount &acct;
	std::string liveChatId;
	const std::unordered_map<std::string, std::string> &thirdPartyEmotes;
	std::function<bool()> canceled;
	std::function<void(bool, const std::string &)> emitState;
	Backoff &backoff;
	bool announced = false;
};

// Confirm the chat once: emit the connected state and flag the live-chat forward as the
// authoritative Super Chat source (so the REST event transport backs off). Idempotent.
void AnnounceOnce(ChatSession &s)
{
	if (!s.announced) {
		s.emitState(true, "");
		s.announced = true;
		s.owner.SetLiveChatActive(true);
	}
}

// While the shared daily-quota gate is closed, surface the outage on the chat state
// and sleep (cancelable) until the reset instant, so neither read loop spends a
// request against a spent quota. Clears `announced` so recovery re-emits the
// connected state once polling resumes. Returns true when the wait was canceled.
bool WaitOutQuotaExhaustion(ChatSession &s)
{
	std::chrono::milliseconds wait{};
	if (!s.owner.QuotaExhausted(&wait)) {
		return false;
	}
	DBG(LogCat::Chat, "youtube: quota exhausted, chat paused %lldms until the reset",
	    static_cast<long long>(wait.count()));
	s.emitState(false, "YouTube API quota exhausted - chat resumes after " + s.owner.QuotaResetLocalTime());
	s.announced = false;
	return CancelableSleep(wait, s.canceled);
}

// Drive the push-based streamList read loop. Returns true to request the .list fallback
// (the transcode endpoint is unavailable: HTTP 404/400); false when the session ended for
// any other reason (cancel, terminal error, or an unrecoverable re-auth with `err` set).
bool RunStreamList(ChatSession &s, std::string &err)
{
	std::string pageToken;
	// The first response object on a COLD connect is backlog; suppress it so the user sees
	// messages from connect onward. This is set false after that first object and never
	// again, so reconnects (which resume from a nextPageToken) emit normally.
	bool firstConnect = true;

	while (!s.canceled()) {
		if (WaitOutQuotaExhaustion(s)) {
			break;
		}
		std::string url = std::string(kLiveChatStreamUrl) + "?liveChatId=" +
				  Http::UrlEncode(s.liveChatId) + "&part=id,snippet,authorDetails&maxResults=" +
				  kStreamMaxResults;
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
					DBG(LogCat::Chat, "youtube streamList: skipped unparseable frame (%ld bytes)",
					    static_cast<long>(objText.size()));
					continue;
				}
				++frameCount;
				AnnounceOnce(s);
				const json &items = Obj(resp, "items");
				const int n = items.is_array() ? static_cast<int>(items.size()) : 0;
				if (firstConnect) {
					firstConnect = false;
					DBG(LogCat::Chat,
					    "youtube streamList: connect frame items=%d (suppressed as backlog)", n);
				} else {
					DBG(LogCat::Chat, "youtube streamList: frame items=%d -> emitting", n);
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
				if (s.canceled()) {
					return false;
				}
			}
			return true;
		};

		std::string errorBody;
		std::string reqErr;
		const long status = s.owner.SendAuthedStreaming(s.acct, req, onChunk, errorBody, reqErr);

		DBG(LogCat::Chat,
		    "youtube streamList: connection ended status=%ld frames=%d bytes=%ld pollAdvised=%ldms",
		    status, frameCount, bytesIn, serverPollMs);

		if (s.canceled()) {
			break;
		}
		if (status == 0) {
			// Transport failure: transient blip, back off and reconnect.
			DBG(LogCat::Chat, "youtube streamList: transport failure (%s), backing off", reqErr.c_str());
			s.emitState(false, reqErr);
			if (CancelableSleep(s.backoff.next(), s.canceled)) {
				break;
			}
			continue;
		}
		if (status == 404 || status == 400) {
			DBG(LogCat::Chat, "youtube: streamList unavailable (HTTP %ld), falling back to list",
			    status);
			return true;
		}
		if (status == 401) {
			// Unrecoverable after a forced refresh: re-auth needed. Terminal.
			DBG(LogCat::Chat, "youtube streamList: terminal HTTP 401 (%s), ending session",
			    reqErr.c_str());
			err = reqErr;
			s.emitState(false, reqErr);
			return false;
		}
		if (status == 403 || status == 429) {
			const std::string reason = status == 403 ? OAuth::YouTubeErrorReason(errorBody)
								 : std::string();
			const OAuth::YouTubeErrorClass cls = OAuth::ClassifyYouTubeError(status, reason);
			if (cls == OAuth::YouTubeErrorClass::QuotaExhausted) {
				// SendAuthedStreaming already armed the shared gate; the loop-top wait
				// reports the outage and sleeps until the reset.
				DBG(LogCat::Chat, "youtube streamList: HTTP %ld (%s) -> quota exhausted", status,
				    reason.c_str());
				continue;
			}
			if (cls == OAuth::YouTubeErrorClass::RateLimited) {
				DBG(LogCat::Chat, "youtube streamList: HTTP %ld (%s) rate-limited, backing off",
				    status, reason.c_str());
				s.emitState(false, "YouTube chat rate-limited, retrying");
				if (CancelableSleep(s.backoff.next(), s.canceled)) {
					break;
				}
				continue;
			}
			DBG(LogCat::Chat, "youtube streamList: terminal HTTP %ld (%s), ending session", status,
			    reason.c_str());
			s.emitState(false, reason.empty() ? "" : "YouTube chat error: " + reason);
			break;
		}
		if (status < 200 || status >= 300) {
			DBG(LogCat::Chat, "youtube streamList: HTTP %ld, backing off", status);
			s.emitState(false, "HTTP " + std::to_string(status));
			if (CancelableSleep(s.backoff.next(), s.canceled)) {
				break;
			}
			continue;
		}

		// 2xx: the server pushed its batch then closed the connection (cleanly, or a
		// mid-batch drop -- reqErr may be set, but every object that arrived was already
		// processed). Resume promptly from the last nextPageToken; honor a larger advisory
		// pollingIntervalMillis if the server sent one.
		s.backoff.reset();
		const long waitMs = std::max<long>(kStreamReconnectFloorMs, serverPollMs);
		DBG(LogCat::Chat, "youtube streamList: batch closed, resuming in %ldms (token=%s)", waitMs,
		    pageToken.empty() ? "none" : "set");
		if (CancelableSleep(std::chrono::milliseconds(waitMs), s.canceled)) {
			break;
		}
	}

	DBG(LogCat::Chat, "youtube streamList: read loop exited (canceled=%d)", s.canceled() ? 1 : 0);
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
		std::string url = std::string(kLiveChatMessagesUrl) + "?liveChatId=" +
				  Http::UrlEncode(s.liveChatId) + "&part=snippet,authorDetails";
		if (!pageToken.empty()) {
			url += "&pageToken=" + Http::UrlEncode(pageToken);
		}

		Http::HttpReq req;
		req.method = "GET";
		req.url = url;

		Http::HttpResponse resp;
		std::string reqErr;
		if (!s.owner.SendAuthed(s.acct, req, resp, reqErr)) {
			// SendAuthed fails only on a transport error (status 0) or an
			// unrecoverable 401. The former is a transient blip worth a backoff
			// retry; the latter is fatal -- re-auth is needed.
			if (resp.status == 0) {
				DBG(LogCat::Chat, "youtube list: transport failure (%s), backing off",
				    reqErr.c_str());
				s.emitState(false, reqErr);
				if (CancelableSleep(s.backoff.next(), s.canceled)) {
					break;
				}
				continue;
			}
			DBG(LogCat::Chat, "youtube list: terminal HTTP %ld (%s), ending session", resp.status,
			    reqErr.c_str());
			err = reqErr;
			s.emitState(false, reqErr);
			return;
		}

		// A 429, or a 403 whose reason is rate-limit class, is transient: a visible
		// reason, a backoff, and a retry. A quota-exhausted 403 stands down until the
		// midnight-Pacific reset via the shared gate. Any other 403 (chat disabled/
		// ended/forbidden) and every 404 (broadcast gone) end the session.
		if (resp.status == 403 || resp.status == 429) {
			const std::string reason =
				resp.status == 403 ? OAuth::YouTubeErrorReason(resp.body) : std::string();
			const OAuth::YouTubeErrorClass cls = OAuth::ClassifyYouTubeError(resp.status, reason);
			if (cls == OAuth::YouTubeErrorClass::QuotaExhausted) {
				// SendAuthed already armed the shared gate; the loop-top wait reports
				// the outage and sleeps until the reset.
				DBG(LogCat::Chat, "youtube list: HTTP %ld (%s) -> quota exhausted", resp.status,
				    reason.c_str());
				continue;
			}
			if (cls == OAuth::YouTubeErrorClass::RateLimited) {
				DBG(LogCat::Chat, "youtube list: HTTP %ld (%s) rate-limited, backing off",
				    resp.status, reason.c_str());
				s.emitState(false, "YouTube chat rate-limited, retrying");
				if (CancelableSleep(s.backoff.next(), s.canceled)) {
					break;
				}
				continue;
			}
			DBG(LogCat::Chat, "youtube list: terminal HTTP %ld (%s), ending session", resp.status,
			    reason.c_str());
			s.emitState(false, reason.empty() ? "" : "YouTube chat error: " + reason);
			break;
		}
		if (resp.status == 404) {
			DBG(LogCat::Chat, "youtube list: HTTP 404 (broadcast gone), ending session");
			s.emitState(false, "");
			break;
		}
		if (resp.status < 200 || resp.status >= 300) {
			DBG(LogCat::Chat, "youtube list: HTTP %ld, backing off", resp.status);
			s.emitState(false, "HTTP " + std::to_string(resp.status));
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
			DBG(LogCat::Chat, "youtube list: first poll items=%d (suppressed as backlog)", n);
		} else {
			DBG(LogCat::Chat, "youtube list: poll items=%d -> emitting", n);
			ProcessChatItems(s.ctx, items, s.liveChatId, s.thirdPartyEmotes, s.canceled);
		}

		if (CancelableSleep(std::chrono::milliseconds(pollMs), s.canceled)) {
			break;
		}
	}

	DBG(LogCat::Chat, "youtube list: poll loop exited (canceled=%d)", s.canceled() ? 1 : 0);
}

} // namespace

bool YouTubeChat::connect(const ChatContext &ctx, OAuth::OAuthAccount &acct, const std::string &channelRef,
			  std::string &err)
{
	// Serialize against an overlapping re-Start: the hub does not join old workers, so a
	// prior connect() might still be unwinding. Held for the whole call (declared before
	// ActiveGuard, so released only after it destructs) -- this guarantees the old
	// generation's ActiveGuard has cleared LiveChatActive before the new generation sets
	// it, so the announced-once flag set/clear on the shared provider can't invert.
	std::lock_guard<std::mutex> run(runMutex_);

	// Defensive: clear the live-chat-active flag up front so it can never survive on a
	// fragile invariant (e.g. a prior run that exited before the RAII guard armed).
	owner_.SetLiveChatActive(false);

	// No active broadcast -> nothing to poll. Clean no-op (empty err = the hub logs
	// nothing); chat.state stays connected=false for this platform via the hub.
	if (channelRef.empty()) {
		err.clear();
		return false;
	}
	const std::string liveChatId = channelRef;
	stop_.store(false, std::memory_order_release);

	// Guarantee the provider's live-chat-active flag is cleared on EVERY exit from this
	// function -- the normal post-loop return, a reconnect give-up, and the unrecoverable
	// 401 that returns from inside the loop -- so the REST event transport resumes
	// supplying Super Chat history the moment this loop stops. It is set true below only
	// once a poll succeeds; the guard's clear is idempotent, so an early exit is safe.
	struct ActiveGuard {
		OAuth::YouTubeProvider &p;
		~ActiveGuard() { p.SetLiveChatActive(false); }
	} activeGuard{owner_};

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

	Backoff backoff(std::chrono::milliseconds(2000), std::chrono::milliseconds(30000));
	ChatSession session{owner_, ctx, acct, liveChatId, thirdPartyEmotes, canceled, emitState, backoff};

	// The proven read path is the classic liveChatMessages.list poll. The push-based
	// streamList (lower quota, ~1s latency) is still under live validation -- on a real
	// 2026-07-24 stream it connected and announced the chat but delivered no messages past
	// the connect frame -- so it is OPT-IN via BRAIDCAST_YOUTUBE_STREAMLIST=1 until proven.
	// The per-frame diagnostic logging in RunStreamList/RunListPoll pins the gap when set.
	const bool tryStreamList = EnvFlag("BRAIDCAST_YOUTUBE_STREAMLIST");
	bool fallback = false;
	if (tryStreamList) {
		DBG(LogCat::Chat, "youtube: opening live chat %s via streamList (opt-in)", liveChatId.c_str());
		fallback = RunStreamList(session, err);
	}
	if (!tryStreamList || (fallback && !canceled())) {
		DBG(LogCat::Chat, "youtube: opening live chat %s via liveChatMessages.list", liveChatId.c_str());
		backoff.reset();
		RunListPoll(session, err);
	}

	// Only bookend with a clean (false, "") when no fatal re-auth error was raised: a fatal
	// 401 already emitted (false, reqErr) inside the loop and set `err`, and the hub logs
	// that -- emitting an empty state after would clobber the reason. A clean cancel / a
	// terminal chat-ended break leaves `err` empty and gets the bookend, matching the
	// Twitch/Kick contract (connect() returns false; empty err = the hub logs nothing).
	if (err.empty()) {
		emitState(false, "");
	}
	return false;
}

bool YouTubeChat::send(OAuth::OAuthAccount &acct, const std::string &text, std::string &err)
{
	// Resolve the active liveChatId off the provider (set by applyMetadata). chat
	// send is only meaningful while a broadcast is live.
	const std::string liveChatId = owner_.chatChannelRef(acct);
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
	if (resp.status < 200 || resp.status >= 300) {
		err = "YouTube chat send failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}
	return true;
}

} // namespace Chat
