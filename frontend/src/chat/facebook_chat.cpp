#include "facebook_chat.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../log.hpp" // DBG / LogCat -- gated path-active logging
#include "../oauth/facebook_graph.hpp"
#include "../oauth/facebook_provider.hpp"
#include "util/http_client.hpp"
#include "util/json_util.hpp"
#include "util/op_error.hpp"
#include "util/time_util.hpp"
#include "seen_ids.hpp"
#include "sse_stream.hpp"
#include "ws_client.hpp" // CancelableSleep / Backoff

namespace Chat {

namespace {

using JsonUtil::Obj;
using JsonUtil::ParseJson;
using JsonUtil::Str;
using TimeUtil::Rfc3339ToEpochMs;

// The push stream. A different host from graph.facebook.com and unversioned: Meta
// documents the server-sent-events surface at exactly this path.
const char *kLiveCommentsHost = "https://streaming-graph.facebook.com/";

// The gap-fill read. `filter=stream` returns replies inline in arrival order rather than
// grouped under their parent, and `live_filter=no_filter` turns off the low-quality
// filtering Meta otherwise applies to a live video's comments -- together they are what
// make this edge's output line up with what the push stream delivers. The edge is
// read-only: Meta documents create/update/delete as unsupported on it.
const char *kBackfillQuery = "?filter=stream&live_filter=no_filter&order=chronological";

// Two parameters Meta documented on the server-sent-events reference pages it has since
// DELETED -- every docs/graph-api/server-sent-events/* URL now redirects -- which leaves
// their accepted values unverifiable. Both are omitted so the server defaults apply. Named
// here so the next change to this request starts from what was actually checked instead of
// rediscovering why they are absent.
constexpr const char *kUnverifiedCommentRateParam = "comment_rate";
constexpr const char *kUnverifiedFieldsParam = "fields";

// Shown for a comment whose `from` Meta withheld. Such a comment is rendered rather than
// dropped: a viewer whose privacy settings hide them from this Page token is an ordinary
// case, and thinning a real chat over it is the worse failure.
constexpr const char *kAnonymousAuthor = "Facebook viewer";

// How far before the session start a comment may be created and still count as live. The
// window exists to keep a broadcast's pre-existing comments out of the pane at go-live,
// and it is deliberately generous: a tight one would suppress REAL messages whenever this
// machine's clock and Meta's disagree, which is worse than showing a minute of history.
constexpr int64_t kBacklogGraceMs = 60000;

// Below this, a 2xx connection that delivered nothing is a reject rather than a stream
// that ran and ended, and reconnecting on the floor would spin.
constexpr long kMinCycleMs = 2000;

// Dead time between a healthy connection closing and the next opening. Meta bills nothing
// per request here, so this is only a guard against a server that closes immediately in a
// loop -- not a quota knob the way the YouTube equivalent is.
constexpr long kReconnectFloorMs = 500;

// The per-connection state the read loop carries across reconnects. References into
// connect()'s frame, which the loop never outlives.
struct CommentSession {
	OAuth::FacebookProvider &owner;
	const ChatContext &ctx;
	std::string liveVideoId;
	// This destination's Page token. It rides the Authorization header the scratch Page
	// account stamps, so it never reaches a URL and never reaches a log line.
	std::string pageToken;
	std::function<bool()> canceled;
	std::function<void(bool, const std::string &)> emitState;
	Backoff &backoff;
	// OAuth::DestinationKey(ctx.dest), rendered once. Every line this loop writes lands in
	// one log that all of an account's chats interleave into, so without this the
	// per-connection lines cannot be attributed to a broadcast. Held as a built string so
	// a gated-off DBG still costs nothing.
	std::string destTag;
	// Every comment id already emitted or deliberately suppressed. Mandatory on this
	// platform rather than defensive: the push stream may replay, and a gap-fill overlaps
	// it by construction.
	SeenIds seen;
	// The newest comment instant seen, in epoch SECONDS -- what a gap-fill passes as
	// `since`. 0 until the first comment arrives, which is also what keeps the first
	// connection from reading any history at all.
	int64_t sinceSec = 0;
	// A comment created before this is pre-existing history, recorded but not shown.
	int64_t backlogCutoffMs = 0;
	bool announced = false;
	// Set by EndSession once the session has ended for good, so connect()'s clean bookend
	// knows not to overwrite the reason. Never cleared: a session has one ending.
	bool terminal = false;
};

// Confirm the chat once, on the first frame that parses rather than on the response
// headers: a socket that opens and then says nothing is the failure shape this transport
// most has to stay honest about. Idempotent.
void AnnounceOnce(CommentSession &s)
{
	if (!s.announced) {
		s.emitState(true, "");
		s.announced = true;
	}
}

// The ONE way the read loop ends for good. It owns every obligation of a terminal exit
// together, so a branch cannot satisfy some and silently drop the rest: the health surface
// gets Failed rather than Reconnecting, the reason reaches the chat pane, `err` carries it
// to the hub's log, and `terminal` stops connect()'s clean bookend from overwriting it. A
// terminal row's reason IS its whole content, so an empty one is substituted rather than
// shown.
void EndSession(CommentSession &s, const std::string &reason, std::string &err)
{
	const std::string text = reason.empty() ? std::string("Facebook comments stopped") : reason;
	s.terminal = true;
	err = text;
	EmitChatTerminal(s.ctx, "facebook", text);
}

// Meta's error envelope ({"error":{"message":...}}). The status stands in when Meta sent no
// message, so a terminal row is never left empty.
std::string GraphErrorReason(const std::string &body, long status)
{
	const json parsed = ParseJson(body);
	const std::string message = Str(Obj(parsed, "error"), "message");
	return message.empty() ? ("HTTP " + std::to_string(status)) : message;
}

// One live_comments frame or /comments row -> the normalized chat frame, or a null json
// when it carries nothing to display.
json NormalizeComment(const json &row, const std::string &liveVideoId, int64_t ts)
{
	const std::string text = Str(row, "message");
	if (text.empty()) {
		return json(nullptr); // sticker- or attachment-only comment: nothing to render
	}
	// `from` is absent ENTIRELY -- not empty -- when the commenter's privacy settings hide
	// them from this Page token. The line is still shown, with no author id: an unknown id
	// is omitted rather than blanked (BuildChatAuthor drops an empty one), so two hidden
	// commenters stay two people in a per-chatter tally instead of collapsing into one.
	const json &from = Obj(row, "from");
	const std::string name = Str(from, "name");
	// Facebook has no emote vocabulary on a comment, so the message is one plain-text
	// fragment and there are no badges to read.
	return BuildChatMessage("facebook", liveVideoId, Str(row, "id"), ts, name.empty() ? kAnonymousAuthor : name,
				Str(from, "id"), std::string(), json::array(),
				json::array({json{{"type", "text"}, {"text", text}}}));
}

// Emit one comment row at most once. True when it reached the pane. Dedupe is
// unconditional here because both of this transport's sources can deliver the same comment.
bool EmitComment(CommentSession &s, const json &row)
{
	if (!row.is_object() || !s.seen.add(Str(row, "id"))) {
		return false;
	}
	// created_time is "2018-08-30T21:11:01+0000": civil fields plus a numeric UTC offset
	// written without a colon. Rfc3339ToEpochMs reads the civil half and treats it as UTC,
	// which is exact for the +0000 Meta documents on this edge.
	const int64_t ts = Rfc3339ToEpochMs(Str(row, "created_time"));
	s.sinceSec = std::max(s.sinceSec, ts / 1000);
	// History the stream handed over on the cold connect: recorded, so a later
	// re-delivery cannot post it either, but not shown.
	if (ts < s.backlogCutoffMs) {
		return false;
	}
	const json msg = NormalizeComment(row, s.liveVideoId, ts);
	if (!msg.is_object()) {
		return false;
	}
	s.ctx.emit(msg);
	return true;
}

// Fill the window a dropped connection lost. There is no server-side resume to ask for --
// the push stream advertises no retry interval and no Last-Event-ID -- so this read IS the
// resume. Best-effort by design: a failed gap-fill costs the few lines that arrived while
// the socket was down, whereas failing the session over it costs the rest of the broadcast.
void GapFill(CommentSession &s)
{
	if (s.sinceSec <= 0) {
		return; // nothing seen yet -> no gap to fill, and no history to import
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url =
		OAuth::GraphUrl(s.liveVideoId + "/comments") + kBackfillQuery + "&since=" + std::to_string(s.sinceSec);

	OAuth::OAuthAccount pageAcct = OAuth::PageAccount(s.owner.id(), s.pageToken);
	Http::HttpResponse resp;
	std::string reqErr;
	if (!s.owner.SendAuthed(pageAcct, req, resp, reqErr) ||
	    !Http::Require2xx(resp, "Facebook comment gap-fill", reqErr)) {
		DBG(LogCat::Chat, "facebook: dest=%s gap-fill failed (%s), resuming the push read anyway",
		    s.destTag.c_str(), Err::Diagnostic(reqErr).c_str());
		return;
	}

	const json parsed = ParseJson(resp.body);
	const json &data = Obj(parsed, "data");
	if (!data.is_array()) {
		DBG(LogCat::Chat, "facebook: dest=%s gap-fill answered without a data array", s.destTag.c_str());
		return;
	}
	int emitted = 0;
	for (const json &row : data) {
		if (s.canceled()) {
			return;
		}
		if (EmitComment(s, row)) {
			++emitted;
		}
	}
	DBG(LogCat::Chat, "facebook: dest=%s gap-fill read %d row(s), %d new", s.destTag.c_str(),
	    static_cast<int>(data.size()), emitted);
}

// One push connection, from open to close. Returns the HTTP status (0 on a transport
// failure, with `reqErr` set); `delivered` counts the comment frames it parsed, which is
// what separates a working stream from one that connects and never speaks.
long RunLiveComments(CommentSession &s, int &delivered, std::string &errorBody, std::string &reqErr)
{
	Http::HttpReq req;
	req.method = "GET";
	// The Page token rides the Authorization header the scratch account below stamps, NOT
	// the `access_token` query parameter Meta's sample uses: a URL is the part of a request
	// that reaches logs and error strings, and this credential must reach neither.
	req.url = std::string(kLiveCommentsHost) + s.liveVideoId + "/live_comments";

	SseStream sse;
	bool overflow = false;
	int frames = 0;

	// Parse and emit each event the moment its bytes finish arriving, so a comment shows up
	// as it is posted rather than when the connection ends. Runs on this worker thread
	// inside curl's write callback.
	auto onChunk = [&](std::string_view chunk) -> bool {
		// Returning false aborts the transfer, which HttpRequestStreaming reports as a
		// clean stop rather than a transport error.
		if (s.canceled()) {
			return false;
		}
		std::vector<SseEvent> events;
		if (!sse.Push(chunk, events)) {
			overflow = true;
			return false;
		}
		if (sse.TakeKeepalive()) {
			DBG(LogCat::Chat, "facebook: dest=%s live-comment keepalive", s.destTag.c_str());
		}
		for (const SseEvent &ev : events) {
			const json row = ParseJson(ev.data);
			if (!row.is_object()) {
				DBG(LogCat::Chat, "facebook: dest=%s skipped an unparseable frame (%ld bytes)",
				    s.destTag.c_str(), static_cast<long>(ev.data.size()));
				continue;
			}
			AnnounceOnce(s);
			++frames;
			EmitComment(s, row);
			if (s.canceled()) {
				return false;
			}
		}
		return true;
	};

	OAuth::OAuthAccount pageAcct = OAuth::PageAccount(s.owner.id(), s.pageToken);
	const long status = s.owner.SendAuthedStreaming(pageAcct, req, onChunk, errorBody, reqErr);
	delivered = frames;
	if (overflow) {
		reqErr = "the live-comment stream sent no event boundary within the frame buffer";
	}
	return status;
}

} // namespace

bool FacebookChat::connect(const ChatContext &ctx, OAuth::OAuthAccount &acct, const std::string &channelRef,
			   std::string &err)
{
	// Serialize against an overlapping re-Start: the hub does not join old workers, so a
	// prior connect() on THIS transport might still be unwinding.
	std::lock_guard<std::mutex> run(runMutex_);

	// No active broadcast -> nothing to read, and nothing here will start one: this
	// transport gets one connect() per go-live. Terminal rather than silent, or the hub's
	// Connecting bookend is the last thing written to this destination's health row and
	// sits there for the rest of the stream. `err` stays empty so the hub still logs
	// nothing.
	if (channelRef.empty()) {
		err.clear();
		EmitChatTerminal(ctx, "facebook", "No active Facebook broadcast to read comments from");
		return false;
	}
	// A live video without its Page token is unreadable: the stored account holds the
	// USER token, which cannot see a Page's comments. Terminal for the same reason.
	const std::string pageToken = owner_.chatPageToken(acct, ctx.dest.profileUuid);
	if (pageToken.empty()) {
		err.clear();
		EmitChatTerminal(ctx, "facebook",
				 "No Facebook Page token for this broadcast, so its comments cannot be read");
		return false;
	}
	stop_.store(false, std::memory_order_release);

	auto canceled = [&] {
		return stop_.load(std::memory_order_acquire) || (ctx.canceled && ctx.canceled());
	};
	auto emitState = [&](bool connected, const std::string &stateErr) {
		EmitChatState(ctx, "facebook", connected, stateErr);
	};

	// Base 2s / cap 30s, matching the YouTube read. Every retry here reopens a stream AND
	// spends a gap-fill request, so the first one is deliberately not immediate, while the
	// cap keeps a long Meta-side outage from leaving the transport idle for minutes after
	// it recovers.
	Backoff backoff(std::chrono::milliseconds(2000), std::chrono::milliseconds(30000));
	CommentSession session{owner_,   ctx,       channelRef, pageToken,
			       canceled, emitState, backoff,    OAuth::DestinationKey(ctx.dest)};
	session.backlogCutoffMs = TimeUtil::NowMs() - kBacklogGraceMs;

	// Tail only: a live-video id addresses one broadcast, and this log is something users
	// paste into issue reports. Enough to tie this session's dest tag to the hub's connect
	// line without reproducing the id.
	const std::string videoTail = channelRef.size() > 6 ? channelRef.substr(channelRef.size() - 6) : channelRef;
	DBG(LogCat::Chat, "facebook: dest=%s video=..%s opening the live-comment stream", session.destTag.c_str(),
	    videoTail.c_str());

	bool firstConnect = true;
	// Consecutive 2xx connections that closed at once having delivered nothing. Only the
	// leading edge is host-logged, so a sustained reject loop stays visible without
	// flooding the log.
	int emptyStreak = 0;

	while (!canceled()) {
		// Skipped on the FIRST connection, which is what keeps a broadcast's existing
		// comment history out of the pane at go-live.
		if (!firstConnect) {
			GapFill(session);
			if (canceled()) {
				break;
			}
		}
		firstConnect = false;

		int delivered = 0;
		std::string errorBody;
		std::string reqErr;
		const auto cycleStart = std::chrono::steady_clock::now();
		const long status = RunLiveComments(session, delivered, errorBody, reqErr);
		reqErr = Err::Diagnostic(reqErr);
		const long cycleMs = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
							       std::chrono::steady_clock::now() - cycleStart)
							       .count());
		DBG(LogCat::Chat,
		    "facebook: dest=%s live-comment connection ended status=%ld comments=%d after %ldms err=%s",
		    session.destTag.c_str(), status, delivered, cycleMs, reqErr.empty() ? "none" : reqErr.c_str());

		if (canceled()) {
			break;
		}
		if (status == 0) {
			DBG(LogCat::Chat, "facebook: dest=%s transport failure (%s), backing off",
			    session.destTag.c_str(), reqErr.c_str());
			emitState(false, reqErr);
			if (CancelableSleep(backoff.next(), canceled)) {
				break;
			}
			continue;
		}
		// The credential, the permission, or the broadcast itself is gone. Nothing a retry
		// recovers, and a row that claims it is reconnecting leaves the user waiting on a
		// chat that will never come back.
		if (status == 400 || status == 401 || status == 403 || status == 404) {
			DBG(LogCat::Chat, "facebook: dest=%s terminal HTTP %ld, ending session",
			    session.destTag.c_str(), status);
			EndSession(session, "Facebook comments stopped: " + GraphErrorReason(errorBody, status), err);
			break;
		}
		if (status < 200 || status >= 300) {
			DBG(LogCat::Chat, "facebook: dest=%s HTTP %ld, backing off", session.destTag.c_str(), status);
			emitState(false, "HTTP " + std::to_string(status));
			if (CancelableSleep(backoff.next(), canceled)) {
				break;
			}
			continue;
		}

		// A 2xx that closed at once carrying nothing is a reject, not a stream that ran
		// and ended, so it takes the growing backoff rather than the reconnect floor --
		// deliberately BOTH predicates, since a short connection that did carry comments
		// is healthy and must not be throttled.
		if (delivered == 0 && cycleMs < kMinCycleMs) {
			const std::chrono::milliseconds wait = backoff.next();
			if (++emptyStreak == 1) {
				HostLog("[chat] facebook: dest=" + session.destTag + " live-comment stream closed in " +
					std::to_string(cycleMs) + "ms without delivering anything; backing off " +
					std::to_string(static_cast<long long>(wait.count())) + "ms");
			} else {
				DBG(LogCat::Chat, "facebook: dest=%s empty %ldms connection #%d, backing off %lldms",
				    session.destTag.c_str(), cycleMs, emptyStreak,
				    static_cast<long long>(wait.count()));
			}
			if (CancelableSleep(wait, canceled)) {
				break;
			}
			continue;
		}
		emptyStreak = 0;
		backoff.reset();
		if (CancelableSleep(std::chrono::milliseconds(kReconnectFloorMs), canceled)) {
			break;
		}
	}

	DBG(LogCat::Chat, "facebook: dest=%s live-comment loop exited (canceled=%d)", session.destTag.c_str(),
	    canceled() ? 1 : 0);

	// The clean (false, "") bookend belongs only to an exit that reported nothing final --
	// a cancel, or a give-up after retries. A session that ended for good already reported
	// its terminal state and reason through EndSession, and an empty state after that would
	// overwrite the one thing the user needs.
	if (!session.terminal) {
		emitState(false, "");
	}
	return false;
}

bool FacebookChat::send(OAuth::OAuthAccount &acct, const std::string &text, std::string &err)
{
	(void)acct;
	(void)text;
	// Meta documents the live-video comments edge as read-only -- it supports no create --
	// and no other edge posts a comment onto a live video as the Page. There is nothing to
	// attempt, so this refuses outright and says why: a send that vanished silently, or one
	// that reported success, would leave the streamer believing their reply went out.
	err = Err::User("Facebook does not accept chat messages through its API (the live-video comments edge is "
			"read-only), so this message was not sent",
			"Facebook does not accept chat messages through its API, so this message was not sent");
	return false;
}

} // namespace Chat
