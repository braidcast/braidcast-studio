#include "event_hub.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "../async_task.hpp"
#include "../bridge.hpp"
#include "../chat/ws_client.hpp" // Chat::CancelableSleep
#include "../log.hpp"
#include "../oauth/provider.hpp"
#include "../oauth/registry.hpp"
#include "../oauth/token_store.hpp"
#include "../overlay/overlay_server.hpp"
#include "../overlay/overlay_store.hpp" // Overlay::Server()

namespace Events {

namespace {

// Pause between real-time reconnect attempts for a transport that advertises no
// poll cadence (a dropped socket / a no-op connect() returning cleanly), so a
// transport that returns immediately can't spin the CPU. Cancel-aware.
constexpr std::chrono::milliseconds kReconnectDelay(1000);

} // namespace

void EventHub::StartAccount(const std::string &providerId, const OAuth::OAuthAccount &acct)
{
	OAuth::StreamProvider *provider = OAuth::Registry().Get(providerId);
	if (!provider) {
		return;
	}
	EventTransport *transport = provider->events();
	if (!transport) {
		return; // provider has no event transport (every provider in 9.2a)
	}

	// Idempotent per providerId: tear down any prior generation (signals its worker +
	// disconnects the transport, outside the lock) before arming a fresh one.
	StopAccount(providerId);

	auto stop = std::make_shared<std::atomic<bool>>(false);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		Active a;
		a.transport = transport;
		a.stop = stop;
		active_[providerId] = std::move(a);
	}

	OAuth::OAuthAccount acctCopy = acct; // the worker owns the account by value

	// The worker owns `acct` by value, the generation cancel flag by shared_ptr, and
	// the transport pointer (owned by the registry-singleton provider, never
	// dangling). It captures the hub (`this`) only via Ingest -- safe because the hub
	// is a singleton living to process exit. All JS emits go through the alive-guarded
	// Bridge::EmitEvent path, never raw CEF.
	AsyncTask::RunAsync([this, providerId, acctCopy, transport, stop]() mutable {
		auto canceled = [stop] { return stop->load(std::memory_order_acquire); };

		EventContext ctx;
		ctx.canceled = canceled;
		ctx.emit = [this, stop](const NormalizedEvent &ev) {
			if (stop->load(std::memory_order_acquire)) {
				return; // generation stopped; drop late emits
			}
			Ingest(ev);
		};

		// 1) One-shot REST backfill: dedupe each result into the store, then emit ONE
		//    events.backfill batch of the events this pass newly added so the dock can
		//    render history in a single pass (later real-time events dedupe against the
		//    same store -> no doubles).
		if (!canceled()) {
			std::vector<NormalizedEvent> seed;
			std::string err;
			bool ok = false;
			try {
				ok = transport->backfill(ctx, acctCopy, seed, err);
			} catch (const std::exception &e) {
				ok = false;
				err = std::string("event backfill crashed: ") + e.what();
			} catch (...) {
				ok = false;
				err = "event backfill crashed: unknown error";
			}
			if (!ok && !err.empty()) {
				HostLog("[events] backfill '" + providerId + "' failed: " + err);
			}
			bool addedAny = false;
			for (const NormalizedEvent &ev : seed) {
				if (Store().Add(ev)) {
					addedAny = true;
				}
			}
			// events.backfill REPLACES the whole feed in the dock, so emit the FULL
			// store snapshot (newest-first), not this account's batch -- a per-account
			// batch would wipe other accounts' already-shown rows. Build it INSIDE the
			// UI lambda so two accounts' concurrent backfills converge on the union: the
			// last-delivered post reads List() live and reflects everything stored.
			if (addedAny && !canceled()) {
				AsyncTask::PostToUi([]() {
					json snapshot = json::array();
					for (const NormalizedEvent &ev : Store().List()) {
						snapshot.push_back(ev.ToJson());
					}
					Bridge::EmitEvent("events.backfill", snapshot);
				});
			}
		}

		// 2) Real-time source + optional poll cadence. connect() blocks until canceled
		//    or the connection drops; when the transport advertises a poll interval,
		//    tick poll() on that cadence between connect returns. Both honor the cancel
		//    token (CancelableSleep) so StopAccount/Shutdown unwind promptly.
		const int pollMs = transport->pollIntervalMs();
		while (!canceled()) {
			std::string err;
			bool ok = false;
			try {
				ok = transport->connect(ctx, acctCopy, err);
			} catch (const std::exception &e) {
				ok = false;
				err = std::string("event transport crashed: ") + e.what();
			} catch (...) {
				ok = false;
				err = "event transport crashed: unknown error";
			}
			if (!ok && !canceled() && !err.empty()) {
				HostLog("[events] transport '" + providerId + "' ended: " + err);
			}
			if (canceled()) {
				break;
			}

			if (pollMs > 0) {
				try {
					transport->poll(ctx, acctCopy);
				} catch (const std::exception &e) {
					HostLog(std::string("[events] poll '") + providerId +
						"' crashed: " + e.what());
				} catch (...) {
					HostLog("[events] poll '" + providerId + "' crashed: unknown error");
				}
				if (Chat::CancelableSleep(std::chrono::milliseconds(pollMs), canceled)) {
					break;
				}
			} else if (Chat::CancelableSleep(kReconnectDelay, canceled)) {
				break;
			}
		}
	});

	HostLog("[events] hub started account: " + providerId);
}

void EventHub::StartConnectedAccounts()
{
	// Mirror the chat hub's account enumeration, but run ONCE at boot rather than on
	// go-live: All() stamps acct.profileUuid, and StartAccount is idempotent per
	// providerId, so this can't double-start an account the connect path also starts.
	for (const auto &entry : OAuth::Tokens().All()) {
		OAuth::OAuthAccount acct = entry.second;
		OAuth::StreamProvider *provider = OAuth::Registry().Get(acct.providerId);
		if (!provider || !provider->isTokenScopeCurrent(acct)) {
			continue;
		}
		StartAccount(acct.providerId, acct);
	}
}

void EventHub::StopAccount(const std::string &providerId)
{
	Active a;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = active_.find(providerId);
		if (it == active_.end()) {
			return;
		}
		a = it->second; // snapshot to signal + disconnect outside the lock
		active_.erase(it);
	}
	if (a.stop) {
		a.stop->store(true, std::memory_order_release);
	}
	if (a.transport) {
		a.transport->disconnect();
	}
}

void EventHub::StopAll()
{
	std::map<std::string, Active> active;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		active = active_; // snapshot the transports to disconnect outside the lock
		active_.clear();
	}
	for (auto &entry : active) {
		if (entry.second.stop) {
			entry.second.stop->store(true, std::memory_order_release);
		}
		if (entry.second.transport) {
			entry.second.transport->disconnect();
		}
	}
}

void EventHub::Ingest(const NormalizedEvent &ev)
{
	if (!Store().Add(ev)) {
		return; // duplicate / no id -> already emitted or unusable; drop
	}
	json payload = ev.ToJson();
	AsyncTask::PostToUi([payload = std::move(payload)]() { Bridge::EmitEvent("events.new", payload); });
	// Phase 9.3: fan the same event to every open overlay widget (SSE). Called off the
	// event worker thread; Broadcast is mutex-guarded + thread-safe. Only reached for a
	// newly-stored (non-duplicate) event, so widgets never double-fire.
	Overlay::Server().Broadcast(ev);
}

EventHub &Hub()
{
	static EventHub hub;
	return hub;
}

EventStore &Store()
{
	static EventStore store;
	return store;
}

} // namespace Events
