#include "chat_hub.hpp"
#include "../event_names.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <utility>

#include "util/async_task.hpp"
#include "util/op_error.hpp"
#include "util/time_util.hpp"
#include "../bridge.hpp"
#include "../log.hpp"
#include "../multistream/OutputBindingStore.hpp"
#include "../multistream/StreamProfileStore.hpp"
#include "../oauth/provider.hpp"
#include "../oauth/registry.hpp"
#include "../oauth/account_store.hpp"
#include "../events/transport_health.hpp"
#include "../obs_bootstrap.hpp"
#include "../overlay/overlay_server.hpp" // OverlayServer::BroadcastChat
#include "../overlay/overlay_store.hpp"  // Overlay::Server()
#include "chat_transport.hpp"

namespace Chat {

namespace {

// Forward one transport-emitted event to the bridge (the multichat dock's feed).
// Runs on the CEF UI thread (posted there by the worker's emit) via the unguarded
// InvokeOnUi trampoline, so an escaped exception here would terminate the process --
// EmitEvent's dump() can throw on a malformed payload (invalid UTF-8); drop the
// frame instead (EmitEvent is already internally guarded, but sits behind the same
// barrier for free). Nothing here may block: every browser source on stream renders
// on this thread, so the overlay SSE fan-out (blocking socket sends) happens on the
// chat transport worker BEFORE this hop (see ctx.emit in ChatHub::Start), never here.
void RouteEmit(const std::string &event, const json &body)
{
	try {
		Bridge::EmitEvent(event, body);
	} catch (...) {
		// malformed chat payload -> drop it rather than crash the UI thread
	}
}

// The destinations targeted by >=1 ENABLED output binding (any canvas): what is actually
// going out over the wire on this go-live. Chat must only connect to (and thus only poll)
// these -- connecting every OAuth-connected account regardless of binding state wastes
// transports and polls a channel's chat the user explicitly disabled.
//
// THE CHAT-TRANSPORT KEYING RULE, applied here in one place:
//   - StreamProvider::broadcastPerDestination() == false (Twitch, Kick): the platform has
//     exactly ONE chat per account, so every enabled profile pointing at that account
//     normalizes to the account-wide destination (empty profileUuid) and collapses into a
//     single set entry. Two profiles on one Twitch channel therefore still produce ONE
//     transport and ONE IRC connection -- unchanged from before this was keyed by
//     destination, and a second transport would double every message and every send.
//   - broadcastPerDestination() == true (YouTube): each profile's broadcast has its own
//     liveChatId, so each enabled profile is its own destination with its own transport.
//     Collapsing them is what left one orientation's chat permanently unread.
// An account whose provider is unknown is skipped -- it can have no transport anyway.
std::set<OAuth::DestinationId> EnabledChatDestinations()
{
	std::set<OAuth::DestinationId> destinations;
	for (const OutputBinding &b : ObsBootstrap::OutputBindings().Bindings().bindings) {
		if (!b.enabled) {
			continue;
		}
		StreamProfile *profile = ObsBootstrap::StreamProfiles().Find(b.profileUuid);
		if (!profile || profile->accountId.empty()) {
			continue;
		}
		const std::optional<OAuth::OAuthAccount> acct = OAuth::Accounts().Get(profile->accountId);
		if (!acct) {
			continue;
		}
		const OAuth::StreamProvider *provider = OAuth::Registry().Get(acct->providerId);
		if (!provider) {
			continue;
		}
		const bool perDestination = provider->broadcastPerDestination();
		destinations.insert(
			OAuth::DestinationId{profile->accountId, perDestination ? b.profileUuid : std::string()});
	}
	return destinations;
}

} // namespace

void ChatHub::Start()
{
	// Idempotent: tear down any prior generation (signals old workers + clears the
	// map) before arming a fresh one, so a re-Start never doubles transports.
	Stop();

	auto stop = std::make_shared<std::atomic<bool>>(false);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = stop;
	}

	int started = 0;
	// One transport per ENABLED destination (see EnabledChatDestinations for the keying
	// rule). Iterating destinations rather than the account store means an account with two
	// live broadcasts gets a reader for each, while a per-channel platform still yields one.
	for (const OAuth::DestinationId &dest : EnabledChatDestinations()) {
		const std::optional<OAuth::OAuthAccount> stored = OAuth::Accounts().Get(dest.accountId);
		if (!stored) {
			continue;
		}
		OAuth::OAuthAccount acct = *stored;
		OAuth::StreamProvider *provider = OAuth::Registry().Get(acct.providerId);
		// A never-finished sign-in must not arm a chat transport (it would show a
		// Multichat chip for a platform the user never connected). IsAccountConnected
		// is the shared gate: registered provider + current scope + refresh token.
		if (!provider || !OAuth::IsAccountConnected(acct)) {
			continue;
		}
		std::shared_ptr<ChatTransport> transport = provider->makeChat(acct);
		if (!transport) {
			continue; // provider has no chat for this account
		}
		const std::string providerId = acct.providerId;
		const std::string channelRef = provider->chatChannelRef(acct, dest.profileUuid);

		// The destination's one emit path toward JS, built here (not inside the worker) so
		// a send can route a local echo of an outbound message through the IDENTICAL
		// pipeline a real incoming message takes (stop-guard, event split, identity stamp,
		// state cache, fallback-id synthesis, overlay fan-out, alive-guarded UI post).
		std::function<void(const json &payload)> emitFn = [this, dest, stop](const json &payload) {
			if (stop->load(std::memory_order_acquire)) {
				return; // generation stopped; drop late emits
			}
			// The payload carries a top-level "event" naming the bridge event; split
			// it from the forwarded body here so the hub stays free of per-platform /
			// per-message-type branches.
			json body = payload;
			std::string event = EventNames::kChatMessage;
			auto ev = body.find("event");
			if (ev != body.end() && ev->is_string()) {
				event = ev->get<std::string>();
				body.erase(ev);
			}
			// Stamp the destination identity onto EVERY frame here, the single fan-out
			// point for all platforms, rather than at each transport's emit sites: a
			// consumer needs to know WHICH account (and which of its broadcasts) a line
			// came from, and `platform` alone cannot say. profileUuid is omitted for an
			// account-wide destination so a per-channel platform's payload gains only the
			// accountId it was always missing.
			body["accountId"] = dest.accountId;
			if (!dest.profileUuid.empty()) {
				body["profileUuid"] = dest.profileUuid;
			}
			// Cache connection state for State() on chat.state events.
			if (event == EventNames::kChatState) {
				std::lock_guard<std::mutex> lock(mutex_);
				auto a = active_.find(dest);
				if (a != active_.end()) {
					if (body.contains("connected") && body["connected"].is_boolean()) {
						a->second.connected = body["connected"].get<bool>();
					}
					a->second.error = body.value("error", std::string());
				}
			}
			if (event == EventNames::kChatMessage) {
				// Single fan-out point for every transport: synthesize a unique
				// fallback id for any chat.message lacking one, so the frontend's
				// keyed list never throws each_key_duplicate. Real ids are left
				// untouched (dedupe relies on them); the monotonic seq guarantees
				// uniqueness even within a single frame.
				auto id = body.find("id");
				const bool missing = id == body.end() || !id->is_string() ||
						     id->get<std::string>().empty();
				if (missing) {
					const std::string platform = body.value("platform", std::string());
					std::string tsStr = "0";
					auto tsIt = body.find("ts");
					if (tsIt != body.end() && tsIt->is_number()) {
						tsStr = std::to_string(tsIt->get<long long>());
					}
					const uint64_t seq = idSeq_.fetch_add(1, std::memory_order_relaxed);
					body["id"] = platform + ":" + tsStr + ":" + std::to_string(seq);
				}
				// Fan chat messages (never connection-state frames) to overlay
				// widgets as a named `chat` SSE event, HERE on the emitting worker
				// rather than after the UI hop (mirrors EventHub::Ingest):
				// BroadcastChat does blocking socket sends (bounded by the overlay
				// server's send timeout), and every browser source on stream renders
				// on the frontend's TID_UI, so one stalled overlay reader would
				// freeze them all. This account's single read worker also keeps its
				// lines in order. dump() can throw on a malformed payload (invalid
				// UTF-8): skip the fan-out and still forward to the (guarded) bridge.
				try {
					Overlay::Server().BroadcastChat(body);
				} catch (...) {
					// malformed chat payload -> skip the overlay fan-out
				}
			}
			AsyncTask::PostToUi([event = std::move(event), body = std::move(body)] { RouteEmit(event, body); });
		};

		{
			std::lock_guard<std::mutex> lock(mutex_);
			Active a;
			a.providerId = providerId;
			a.dest = dest;
			a.transport = transport;
			a.emit = emitFn;
			active_[dest] = std::move(a);
		}

		// The worker owns `acct` by value, the generation cancel flag by shared_ptr, and
		// a shared_ptr COPY of the transport. The copy keeps the transport alive until the
		// worker itself exits, so a per-account Stop() (which drops the hub's ref and calls
		// disconnect()) can't use-after-free an in-flight connect(). It captures the hub
		// (`this`) only for mutex-guarded status writeback -- safe because the hub is a
		// singleton living to process exit. All JS emits go through Bridge::EmitEvent
		// (alive-guarded), never raw CEF.
		AsyncTask::RunAsync([this, dest, providerId, channelRef, acct, transport, stop, emitFn]() mutable {
			ChatContext ctx;
			ctx.dest = dest;
			ctx.canceled = [stop] {
				return stop->load(std::memory_order_acquire);
			};
			ctx.emit = emitFn;
			// Route this transport's health transitions to the shared aggregator, keyed by
			// platform. Dropped once the generation stops so a late worker report can't
			// override the Disconnected that Stop() writes as the authoritative terminal.
			ctx.reportHealth = [providerId, stop](Transports::TransportHealth::State st,
							      const std::string &healthErr) {
				if (stop->load(std::memory_order_acquire)) {
					return;
				}
				Transports::Health().Report(Transports::ChatTransportId(providerId), st, healthErr);
			};

			std::string err;
			bool ok = false;
			DBG(LogCat::Chat, "connecting transport '%s' (channel %s)", providerId.c_str(),
			    channelRef.c_str());
			// Connecting bookend: the transport reports Connected/Reconnecting itself via
			// EmitChatState once its socket is up or drops.
			ctx.reportHealth(Transports::TransportHealth::State::Connecting, "");
			try {
				ok = transport->connect(ctx, acct, channelRef, err);
			} catch (const std::exception &e) {
				err = std::string("chat transport crashed: ") + e.what();
			} catch (...) {
				err = "chat transport crashed: unknown error";
			}
			if (!ok && !ctx.canceled() && !err.empty()) {
				HostLog("[chat] transport '" + providerId + "' ended: " + Err::Diagnostic(err));
			}
		});
		++started;
	}
	HostLog("[chat] hub started: " + std::to_string(started) + " transport(s)");
}

void ChatHub::Stop()
{
	std::shared_ptr<std::atomic<bool>> stop;
	std::map<OAuth::DestinationId, Active> active;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop = stop_;
		active = active_; // snapshot the transport pointers to disconnect outside the lock
		active_.clear();
		stop_.reset();
	}
	if (stop) {
		stop->store(true, std::memory_order_release); // signal every loop of this generation
	}
	for (auto &entry : active) {
		if (entry.second.transport) {
			entry.second.transport->disconnect();
		}
		// Authoritative terminal for this transport's health: the generation flag is now
		// set, so its worker's own late reports are dropped and cannot override this.
		Transports::Health().Report(Transports::ChatTransportId(entry.second.providerId),
					    Transports::TransportHealth::State::Disconnected);
	}
}

void ChatHub::SendToPlatforms(const std::vector<std::string> &platforms, const std::string &text)
{
	SendMatching(
		[&platforms](const Active &a) {
			return platforms.empty() ||
			       std::find(platforms.begin(), platforms.end(), a.providerId) != platforms.end();
		},
		text);
}

void ChatHub::SendToDestination(const OAuth::DestinationId &dest, const std::string &text)
{
	SendMatching([&dest](const Active &a) { return a.dest == dest; }, text);
}

void ChatHub::SendMatching(const std::function<bool(const Active &)> &match, const std::string &text)
{
	std::vector<Active> targets;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const auto &entry : active_) {
			if (match(entry.second)) {
				targets.push_back(entry.second);
			}
		}
	}

	for (auto &t : targets) {
		const OAuth::DestinationId dest = t.dest;
		const std::string providerId = t.providerId;
		std::shared_ptr<ChatTransport> transport = t.transport;
		std::function<void(const json &payload)> emit = t.emit;
		const std::string msg = text;
		// One worker per send so a slow REST send never blocks the caller. The worker
		// captures the shared_ptr (not a raw ptr) so a concurrent Stop() can't free the
		// transport mid-send.
		AsyncTask::RunAsync([dest, providerId, transport, emit, msg]() {
			// Load the account fresh from the store so ensureFresh stays the sole token
			// writer (no pre-call snapshot writeback -- mirrors the streamMeta.* path).
			std::optional<OAuth::OAuthAccount> stored = OAuth::Accounts().Get(dest.accountId);
			if (!stored) {
				return;
			}
			OAuth::OAuthAccount acct = *stored;
			std::string err;
			bool ok = false;
			try {
				ok = transport->send(acct, msg, err);
			} catch (const std::exception &e) {
				err = std::string("send failed: ") + e.what();
			} catch (...) {
				err = "send failed: unknown error";
			}
			if (!ok) {
				// chat.state carries a human-readable line; unpack a quota envelope
				// rather than pushing raw envelope JSON at the dock. Identity is
				// stamped to match the frames that come through emitFn -- this one
				// reports a send failure and so cannot route through the read path.
				AsyncTask::PostToUi([dest, providerId, err = Err::Diagnostic(err)] {
					json body = json{{"platform", providerId},
							 {"accountId", dest.accountId},
							 {"connected", true},
							 {"error", err}};
					if (!dest.profileUuid.empty()) {
						body["profileUuid"] = dest.profileUuid;
					}
					Bridge::EmitEvent(EventNames::kChatState, body);
				});
				return;
			}
			if (!transport->reflectsOwnSend() && emit) {
				// A platform whose read transport never reflects the sender's own
				// message (Twitch IRC) would leave the streamer's send invisible in
				// their own pane, so echo it locally through the account's regular
				// emit path -- the identical pipeline a real incoming message takes
				// (overlay fan-out, alive-guarded UI post). No "id" on purpose: the
				// emit path's fallback-id synthesis mints a unique one from idSeq_,
				// and a non-reflecting platform has no real id to dedupe against.
				const std::string name = acct.displayName.empty() ? acct.login : acct.displayName;
				emit(json{
					{"event", EventNames::kChatMessage},
					{"platform", providerId},
					{"channelId", transport->channelId()},
					{"ts", TimeUtil::NowMs()},
					{"author", json{{"name", name}, {"color", ""}, {"badges", json::array()}}},
					{"fragments", json::array({json{{"type", "text"}, {"text", msg}}})},
				});
			}
		});
	}
}

json ChatHub::State()
{
	json arr = json::array();
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &entry : active_) {
		json row = json{
			{"platform", entry.second.providerId},
			{"accountId", entry.first.accountId},
			{"connected", entry.second.connected},
			{"error", entry.second.error},
		};
		// Omitted for an account-wide destination, so a per-channel platform's row keeps
		// exactly the shape it had plus the accountId it always lacked.
		if (!entry.first.profileUuid.empty()) {
			row["profileUuid"] = entry.first.profileUuid;
		}
		arr.push_back(std::move(row));
	}
	return arr;
}

ChatHub &Hub()
{
	static ChatHub hub;
	return hub;
}

} // namespace Chat
