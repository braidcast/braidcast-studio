#include "youtube_chat.hpp"
#include "../event_names.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>

#include "../events/event_hub.hpp"   // Events::Hub().Ingest for monetization/membership events
#include "../events/event_model.hpp" // Events::NormalizedEvent
#include "util/http_client.hpp"
#include "util/json_util.hpp"
#include "../oauth/youtube_provider.hpp"
#include "util/time_util.hpp"
#include "third_party_emotes.hpp"
#include "ws_client.hpp" // CancelableSleep / Backoff

namespace Chat {

namespace {

const char *kLiveChatMessagesUrl = "https://www.googleapis.com/youtube/v3/liveChat/messages";

// liveChatMessages.list omits pollingIntervalMillis on rare responses; fall back
// to a conservative interval and never poll faster than this floor (quota guard).
constexpr long kDefaultPollMs = 5000;
constexpr long kMinPollMs = 1500;

using JsonUtil::Bool;
using JsonUtil::NumLoose;
using JsonUtil::Obj;
using JsonUtil::ParseJson;
using JsonUtil::Str;
using TimeUtil::Rfc3339ToEpochMs;

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

	std::string pageToken;
	bool firstPoll = true;
	bool announced = false;
	Backoff backoff(std::chrono::milliseconds(2000), std::chrono::milliseconds(30000));

	while (!canceled()) {
		std::string url = std::string(kLiveChatMessagesUrl) + "?liveChatId=" + Http::UrlEncode(liveChatId) +
				  "&part=snippet,authorDetails";
		if (!pageToken.empty()) {
			url += "&pageToken=" + Http::UrlEncode(pageToken);
		}

		Http::HttpReq req;
		req.method = "GET";
		req.url = url;

		Http::HttpResponse resp;
		std::string reqErr;
		if (!owner_.SendAuthed(acct, req, resp, reqErr)) {
			// SendAuthed fails only on a transport error (status 0) or an
			// unrecoverable 401 after a forced refresh. The latter is fatal --
			// re-auth is needed; the former is a transient blip worth a backoff retry.
			if (resp.status == 0) {
				emitState(false, reqErr);
				if (CancelableSleep(backoff.next(), canceled)) {
					break;
				}
				continue;
			}
			err = reqErr;
			emitState(false, reqErr);
			return false;
		}

		// 403 (chat disabled/ended/quota) or 404 (broadcast gone) end the session
		// cleanly -- the chat no longer exists, so there is nothing to retry.
		if (resp.status == 403 || resp.status == 404) {
			emitState(false, "");
			break;
		}
		if (resp.status < 200 || resp.status >= 300) {
			emitState(false, "HTTP " + std::to_string(resp.status));
			if (CancelableSleep(backoff.next(), canceled)) {
				break;
			}
			continue;
		}

		backoff.reset();
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

		if (!announced) {
			emitState(true, "");
			announced = true;
			// The live chat is confirmed polling this broadcast: from here the chat forward
			// is the authoritative Super Chat source, so the REST transport backs off.
			owner_.SetLiveChatActive(true);
		}

		// First response is backlog: keep only the cursor, emit nothing, so the user
		// sees messages from connect onward rather than a wall of history.
		if (firstPoll) {
			firstPoll = false;
		} else if (j.is_object() && j.contains("items") && j["items"].is_array()) {
			for (const json &item : j["items"]) {
				if (canceled()) {
					break;
				}
				// Chat first, exactly as before: a plain message emits a chat line; a
				// Super Chat / membership item still emits its chat line (it carries text).
				const json msg = NormalizeItem(item, liveChatId, thirdPartyEmotes);
				if (msg.is_object()) {
					ctx.emit(msg);
				}
				// Then, IN ADDITION, forward monetization/membership types into the events
				// feed. YouTube has no real-time event socket, so this live-chat sink is the
				// only push source for Super Chats/memberships (the REST transport covers
				// pre-live history). The store dedupes against backfill/poll.
				Events::NormalizedEvent ev;
				if (BuildEventFromChat(item, ev)) {
					Events::Hub().Ingest(ev);
				}
			}
		}

		if (CancelableSleep(std::chrono::milliseconds(pollMs), canceled)) {
			break;
		}
	}

	emitState(false, "");
	// Match Twitch/Kick: connect() returns false on a clean cancel/end (err stays
	// empty, so the hub logs nothing). Contract parity across transports.
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
