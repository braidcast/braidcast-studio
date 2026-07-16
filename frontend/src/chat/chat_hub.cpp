#include "chat_hub.hpp"
#include "../event_names.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <utility>

#include "util/async_task.hpp"
#include "../bridge.hpp"
#include "../log.hpp"
#include "../multistream/OutputBindingStore.hpp"
#include "../multistream/StreamProfileStore.hpp"
#include "../oauth/provider.hpp"
#include "../oauth/registry.hpp"
#include "../oauth/account_store.hpp"
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

// Account ids targeted by >=1 ENABLED output binding (any canvas): the destinations
// actually going out over the wire on this go-live. Chat must only connect to (and
// thus only poll) these -- connecting every OAuth-connected account regardless of
// binding state wastes transports and polls a channel's chat the user explicitly
// disabled.
std::set<std::string> EnabledChatAccountIds()
{
	std::set<std::string> ids;
	for (const OutputBinding &b : ObsBootstrap::OutputBindings().Bindings().bindings) {
		if (!b.enabled) {
			continue;
		}
		StreamProfile *profile = ObsBootstrap::StreamProfiles().Find(b.profileUuid);
		if (profile && !profile->accountId.empty()) {
			ids.insert(profile->accountId);
		}
	}
	return ids;
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
	const std::set<std::string> enabledAccountIds = EnabledChatAccountIds();
	for (const auto &entry : OAuth::Accounts().All()) {
		OAuth::OAuthAccount acct = entry.second;
		const std::string accountId = OAuth::AccountId(acct);
		// No enabled destination targets this account -- don't connect (or poll) its
		// chat. Checked before IsAccountConnected purely as the cheaper filter first.
		if (!enabledAccountIds.count(accountId)) {
			continue;
		}
		OAuth::StreamProvider *provider = OAuth::Registry().Get(acct.providerId);
		// A never-finished sign-in must not arm a chat transport (it would show a
		// Multichat chip for a platform the user never connected). IsAccountConnected
		// is the shared gate: registered provider + current scope + refresh token.
		if (!OAuth::IsAccountConnected(acct)) {
			continue;
		}
		std::shared_ptr<ChatTransport> transport = provider->makeChat(acct);
		if (!transport) {
			continue; // provider has no chat for this account
		}
		const std::string providerId = acct.providerId;
		const std::string channelRef = provider->chatChannelRef(acct);

		{
			std::lock_guard<std::mutex> lock(mutex_);
			Active a;
			a.providerId = providerId;
			a.transport = transport;
			active_[accountId] = std::move(a);
		}

		// The worker owns `acct` by value, the generation cancel flag by shared_ptr, and
		// a shared_ptr COPY of the transport. The copy keeps the transport alive until the
		// worker itself exits, so a per-account Stop() (which drops the hub's ref and calls
		// disconnect()) can't use-after-free an in-flight connect(). It captures the hub
		// (`this`) only for mutex-guarded status writeback -- safe because the hub is a
		// singleton living to process exit. All JS emits go through Bridge::EmitEvent
		// (alive-guarded), never raw CEF.
		AsyncTask::RunAsync([this, accountId, providerId, channelRef, acct, transport, stop]() mutable {
			ChatContext ctx;
			ctx.canceled = [stop] {
				return stop->load(std::memory_order_acquire);
			};
			ctx.emit = [this, accountId, stop](const json &payload) {
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
				// Cache connection state for State() on chat.state events.
				if (event == EventNames::kChatState) {
					std::lock_guard<std::mutex> lock(mutex_);
					auto a = active_.find(accountId);
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
					// widgets as a named `chat` SSE event, HERE on the transport worker
					// rather than after the UI hop (mirrors EventHub::Ingest):
					// BroadcastChat does blocking socket sends (bounded by the overlay
					// server's send timeout), and every browser source on stream renders
					// on the frontend's TID_UI, so one stalled overlay reader would
					// freeze them all. This account's single worker also keeps its lines
					// in order. dump() can throw on a malformed payload (invalid UTF-8):
					// skip the fan-out and still forward to the (guarded) bridge.
					try {
						Overlay::Server().BroadcastChat(body);
					} catch (...) {
						// malformed chat payload -> skip the overlay fan-out
					}
				}
				AsyncTask::PostToUi(
					[event = std::move(event), body = std::move(body)] { RouteEmit(event, body); });
			};

			std::string err;
			bool ok = false;
			DBG(LogCat::Chat, "connecting transport '%s' (channel %s)", providerId.c_str(),
			    channelRef.c_str());
			try {
				ok = transport->connect(ctx, acct, channelRef, err);
			} catch (const std::exception &e) {
				err = std::string("chat transport crashed: ") + e.what();
			} catch (...) {
				err = "chat transport crashed: unknown error";
			}
			if (!ok && !ctx.canceled() && !err.empty()) {
				HostLog("[chat] transport '" + providerId + "' ended: " + err);
			}
		});
		++started;
	}
	HostLog("[chat] hub started: " + std::to_string(started) + " transport(s)");
}

void ChatHub::Stop()
{
	std::shared_ptr<std::atomic<bool>> stop;
	std::map<std::string, Active> active;
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
	}
}

void ChatHub::SendToPlatforms(const std::vector<std::string> &platforms, const std::string &text)
{
	std::vector<std::pair<std::string, Active>> targets; // (accountId, Active)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const auto &entry : active_) {
			const bool match = platforms.empty() || std::find(platforms.begin(), platforms.end(),
									  entry.second.providerId) != platforms.end();
			if (match) {
				targets.emplace_back(entry.first, entry.second);
			}
		}
	}

	for (auto &t : targets) {
		const std::string accountId = t.first;
		const std::string providerId = t.second.providerId;
		std::shared_ptr<ChatTransport> transport = t.second.transport;
		const std::string msg = text;
		// One worker per send so a slow REST send never blocks the caller. The worker
		// captures the shared_ptr (not a raw ptr) so a concurrent Stop() can't free the
		// transport mid-send.
		AsyncTask::RunAsync([accountId, providerId, transport, msg]() {
			// Load the account fresh from the store so ensureFresh stays the sole token
			// writer (no pre-call snapshot writeback -- mirrors the streamMeta.* path).
			std::optional<OAuth::OAuthAccount> stored = OAuth::Accounts().Get(accountId);
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
				AsyncTask::PostToUi([providerId, err] {
					Bridge::EmitEvent(EventNames::kChatState, json{{"platform", providerId},
										       {"connected", true},
										       {"error", err}});
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
		arr.push_back(json{
			{"platform", entry.second.providerId},
			{"connected", entry.second.connected},
			{"error", entry.second.error},
		});
	}
	return arr;
}

ChatHub &Hub()
{
	static ChatHub hub;
	return hub;
}

} // namespace Chat
