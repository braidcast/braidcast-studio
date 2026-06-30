#include "chat_hub.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "../async_task.hpp"
#include "../bridge.hpp"
#include "../log.hpp"
#include "../oauth/provider.hpp"
#include "../oauth/registry.hpp"
#include "../oauth/token_store.hpp"
#include "chat_transport.hpp"

namespace Chat {

namespace {

// Route one transport-emitted payload to its bridge event. The payload carries a
// top-level "event" naming the bridge event; strip it and forward the rest, so
// the hub stays free of per-platform / per-message-type branches. Runs on the UI
// thread (posted there by the worker's emit) via the alive-guarded EmitEvent.
void RouteEmit(const json &payload)
{
	std::string event = "chat.message";
	json body = payload;
	auto it = body.find("event");
	if (it != body.end() && it->is_string()) {
		event = it->get<std::string>();
		body.erase(it);
	}
	Bridge::EmitEvent(event, body);
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
	for (const auto &entry : OAuth::Tokens().All()) {
		const std::string profileUuid = entry.first;
		OAuth::OAuthAccount acct = entry.second; // copy: All() stamps acct.profileUuid
		OAuth::StreamProvider *provider = OAuth::Registry().Get(acct.providerId);
		if (!provider || !provider->isTokenScopeCurrent(acct)) {
			continue;
		}
		ChatTransport *transport = provider->chat();
		if (!transport) {
			continue; // provider has no chat (every provider in Task 2)
		}
		const std::string providerId = acct.providerId;
		const std::string channelRef = provider->chatChannelRef(acct);

		{
			std::lock_guard<std::mutex> lock(mutex_);
			Active a;
			a.providerId = providerId;
			a.transport = transport;
			active_[profileUuid] = std::move(a);
		}

		// The worker owns `acct` by value, the generation cancel flag by shared_ptr,
		// and the transport pointer (owned by the registry-singleton provider, never
		// dangling). It captures the hub (`this`) only for mutex-guarded status
		// writeback -- safe because the hub is a singleton living to process exit. All
		// JS emits go through Bridge::EmitEvent (alive-guarded), never raw CEF.
		AsyncTask::RunAsync([this, profileUuid, providerId, channelRef, acct, transport, stop]() mutable {
			ChatContext ctx;
			ctx.canceled = [stop] { return stop->load(std::memory_order_acquire); };
			ctx.emit = [this, profileUuid, stop](const json &payload) {
				if (stop->load(std::memory_order_acquire)) {
					return; // generation stopped; drop late emits
				}
				// Cache connection state for State() on chat.state events.
				auto ev = payload.find("event");
				if (ev != payload.end() && ev->is_string() &&
				    ev->get<std::string>() == "chat.state") {
					std::lock_guard<std::mutex> lock(mutex_);
					auto a = active_.find(profileUuid);
					if (a != active_.end()) {
						if (payload.contains("connected") &&
						    payload["connected"].is_boolean()) {
							a->second.connected = payload["connected"].get<bool>();
						}
						a->second.error = payload.value("error", std::string());
					}
				}
				json p = payload;
				AsyncTask::PostToUi([p = std::move(p)] { RouteEmit(p); });
			};

			std::string err;
			bool ok = false;
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
	std::vector<std::pair<std::string, Active>> targets; // (profileUuid, Active)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const auto &entry : active_) {
			const bool match =
				platforms.empty() ||
				std::find(platforms.begin(), platforms.end(), entry.second.providerId) !=
					platforms.end();
			if (match) {
				targets.emplace_back(entry.first, entry.second);
			}
		}
	}

	for (auto &t : targets) {
		const std::string profileUuid = t.first;
		const std::string providerId = t.second.providerId;
		ChatTransport *transport = t.second.transport;
		const std::string msg = text;
		// One worker per send so a slow REST send never blocks the caller.
		AsyncTask::RunAsync([profileUuid, providerId, transport, msg]() {
			// Load the account fresh from the store so ensureFresh stays the sole token
			// writer (no pre-call snapshot writeback -- mirrors the streamMeta.* path).
			std::optional<OAuth::OAuthAccount> stored = OAuth::Tokens().Get(profileUuid);
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
					Bridge::EmitEvent("chat.state", json{{"platform", providerId},
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
