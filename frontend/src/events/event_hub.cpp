#include "event_hub.hpp"
#include "../event_names.hpp"

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
#include "../oauth/account_store.hpp"
#include "../overlay/overlay_server.hpp"
#include "../overlay/overlay_store.hpp" // Overlay::Server()

namespace Events {

namespace {

// Pause between real-time reconnect attempts for a transport that advertises no
// poll cadence (a dropped socket / a no-op connect() returning cleanly), so a
// transport that returns immediately can't spin the CPU. Cancel-aware.
constexpr std::chrono::milliseconds kReconnectDelay(1000);

} // namespace

void EventHub::StartAccount(const std::string &accountId, const OAuth::OAuthAccount &acct)
{
	// The provider is resolved off the account (providerId), not the key -- the key is
	// now the accountId. Log lines keep using providerId for readability.
	const std::string providerId = acct.providerId;
	OAuth::StreamProvider *provider = OAuth::Registry().Get(providerId);
	if (!provider) {
		return;
	}
	std::shared_ptr<EventTransport> transport = provider->makeEvents(acct);
	if (!transport) {
		return; // provider has no event transport for this account
	}

	// Idempotent per accountId: displace any prior generation atomically. Reading the
	// old entry and installing the new one happen under ONE lock so two concurrent
	// StartAccount(sameId) (e.g. boot's StartConnectedAccounts racing the oauth.connect
	// path) can't both find nothing and both insert -- exactly one survives, and the
	// displaced generation is always signalled. Its worker holds its own transport
	// shared_ptr copy, so signalling + disconnecting it outside the lock can't
	// use-after-free an in-flight connect().
	auto stop = std::make_shared<std::atomic<bool>>(false);
	Active prev;
	bool hadPrev = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = active_.find(accountId);
		if (it != active_.end()) {
			prev = it->second;
			hadPrev = true;
		}
		Active a;
		a.transport = transport;
		a.stop = stop;
		active_[accountId] = std::move(a);
	}
	if (hadPrev) {
		if (prev.stop) {
			prev.stop->store(true, std::memory_order_release);
		}
		if (prev.transport) {
			prev.transport->disconnect();
		}
	}

	OAuth::OAuthAccount acctCopy = acct; // the worker owns the account by value

	// The worker owns `acct` by value, the generation cancel flag by shared_ptr, and a
	// shared_ptr COPY of the transport. The copy keeps the transport alive until the
	// worker itself exits, so a per-account StopAccount() (which drops the hub's ref and
	// calls disconnect()) can't use-after-free an in-flight connect(). It captures the
	// hub (`this`) only via Ingest -- safe because the hub is a singleton living to
	// process exit. All JS emits go through the alive-guarded Bridge::EmitEvent path,
	// never raw CEF.
	AsyncTask::RunAsync([this, providerId, acctCopy, transport, stop]() mutable {
		auto canceled = [stop] { return stop->load(std::memory_order_acquire); };

		EventContext ctx;
		ctx.canceled = canceled;
		// Set by a transport just before it returns a permanent failure (see
		// EventContext::markFatal); the reconnect loop below reads it to stop retrying.
		bool fatal = false;
		ctx.markFatal = [&fatal] { fatal = true; };
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
					Bridge::EmitEvent(EventNames::kEventsBackfill, snapshot);
				});
			}
		}

		// 2) Real-time source + optional poll cadence. connect() blocks until canceled
		//    or the connection drops; when the transport advertises a poll interval,
		//    tick poll() on that cadence between connect returns. Both honor the cancel
		//    token (CancelableSleep) so StopAccount/Shutdown unwind promptly.
		const int pollMs = transport->pollIntervalMs();
		while (!canceled()) {
			fatal = false;
			std::string err;
			bool ok = false;
			DBG(LogCat::Events, "connecting transport '%s'", providerId.c_str());
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
			if (!ok && fatal) {
				// Permanent failure (misconfigured account / missing WS support): retrying
				// would just spin the loop. Stop until the next explicit Start re-arms it.
				HostLog("[events] transport '" + providerId +
					"' permanently failed; not retrying until reconnect");
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
	// go-live: StartAccount is idempotent per accountId, so this can't double-start an
	// account the connect path also starts.
	for (const auto &entry : OAuth::Accounts().All()) {
		OAuth::OAuthAccount acct = entry.second;
		// Shared connection gate: a never-finished sign-in must not arm an events
		// transport. See OAuth::IsAccountConnected.
		if (!OAuth::IsAccountConnected(acct)) {
			continue;
		}
		StartAccount(OAuth::AccountId(acct), acct);
	}
}

void EventHub::StopAccount(const std::string &accountId)
{
	Active a;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = active_.find(accountId);
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
	AsyncTask::PostToUi([payload = std::move(payload)]() { Bridge::EmitEvent(EventNames::kEventsNew, payload); });
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
