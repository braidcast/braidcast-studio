#include "event_hub.hpp"
#include "../event_names.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "util/async_task.hpp"
#include "../bridge.hpp"
#include "../chat/chat_archive.hpp" // Chat::ModerationOp, Chat::Redaction
#include "../chat/ws_client.hpp"    // Chat::CancelableSleep
#include "../log.hpp"
#include "../oauth/provider.hpp"
#include "../oauth/registry.hpp"
#include "../oauth/account_store.hpp"
#include "../overlay/overlay_server.hpp"
#include "../overlay/overlay_store.hpp" // Overlay::Server()
#include "transport_health.hpp"

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
	AsyncTask::RunAsync([this, accountId, providerId, acctCopy, transport, stop]() mutable {
		auto canceled = [stop] {
			return stop->load(std::memory_order_acquire);
		};

		EventContext ctx;
		ctx.canceled = canceled;
		// Set by a transport just before it returns a permanent failure (see
		// EventContext::markFatal); the reconnect loop below reads it to stop retrying.
		bool fatal = false;
		ctx.markFatal = [&fatal] {
			fatal = true;
		};
		ctx.emit = [this, accountId, stop](const NormalizedEvent &ev) {
			if (stop->load(std::memory_order_acquire)) {
				return; // generation stopped; drop late emits
			}
			// Stamp the owning account here, the single point every transport's emit
			// funnels through, rather than at each transport's construction sites: two
			// accounts on one platform are otherwise indistinguishable downstream. A
			// transport that already attributed the event (the YouTube chat sink names the
			// exact broadcast) is left alone.
			NormalizedEvent stamped = ev;
			if (stamped.accountId.empty()) {
				stamped.accountId = accountId;
			}
			Ingest(stamped);
		};
		// Route health transitions to the shared aggregator, keyed by this ACCOUNT's
		// account-wide destination -- the hub runs one event transport per account, and a
		// platform-wide id lets two accounts on one platform overwrite each other's state.
		// Dropped once the generation stops so a late report can't override the Disconnected
		// that StopAccount/StopAll writes as the authoritative terminal.
		const std::string healthId = Transports::EventsTransportId(OAuth::AccountDestination(accountId));
		ctx.reportHealth = [healthId, stop](Transports::TransportHealth::State st,
						    const std::string &healthErr) {
			if (stop->load(std::memory_order_acquire)) {
				return;
			}
			Transports::Health().Report(healthId, st, healthErr);
		};

		// 1) One-shot REST backfill: dedupe each result into the store, then emit ONE
		//    events.backfill batch of the events this pass newly added so an open dock
		//    can merge them into its window (later real-time events dedupe against the
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
			// Admitted and posted under one hold of the admission lock, so a removal lands
			// wholly before the batch (it is scrubbed) or wholly after (its events.redacted
			// follows the batch).
			std::lock_guard<std::mutex> admission(admitMutex_);
			json added = json::array();
			for (NormalizedEvent &ev : seed) {
				// Stamped like an emitted event, so a removal on this account reaches it.
				if (Admit(ev, accountId)) {
					added.push_back(ev.ToJson());
				}
			}
			// Only what this pass newly stored: the dock merges each event into its window
			// at its own place in time, so rows already shown -- this account's or
			// another's -- are left alone.
			if (!added.empty() && !canceled()) {
				AsyncTask::PostToUi([added = std::move(added)]() {
					Bridge::EmitEvent(EventNames::kEventsBackfill, added);
				});
			}
		}

		// 2) Real-time source + optional poll cadence. connect() blocks until canceled
		//    or the connection drops; when the transport advertises a poll interval,
		//    tick poll() on that cadence between connect returns (re-read per tick so a
		//    live-aware transport can stretch it while nothing is streaming). Both honor
		//    the cancel token (CancelableSleep) so StopAccount/Shutdown unwind promptly.
		const bool polls = transport->pollIntervalMs() > 0;
		// Connecting bookend: a real-time transport reports Connected itself once its
		// socket handshakes; the reconnect below re-marks Reconnecting on a drop.
		ReportHealth(ctx, Transports::TransportHealth::State::Connecting);
		// True once a poll-only transport has idled cleanly and is re-entering on its own
		// cadence, so the line below can say so. Saying "connecting" every cycle reads as a
		// reconnect loop and was misread as exactly that: 76 of those lines in an hour were
		// the YouTube REST transport keeping its 90s cadence, with nothing wrong. A genuine
		// drop clears this, so a real reconnect still announces itself as one.
		bool idlingOnCadence = false;
		while (!canceled()) {
			fatal = false;
			std::string err;
			bool ok = false;
			if (idlingOnCadence) {
				DBG(LogCat::Events, "polling transport '%s'", providerId.c_str());
			} else {
				DBG(LogCat::Events, "connecting transport '%s'", providerId.c_str());
			}
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
				ReportHealth(ctx, Transports::TransportHealth::State::Failed, err);
				break;
			}
			// A non-fatal drop (socket closed / network blip): the loop backs off and
			// reconnects below. A clean return (ok) is a poll-only transport idling
			// between ticks -- leave its Connected state untouched.
			if (!ok) {
				ReportHealth(ctx, Transports::TransportHealth::State::Reconnecting, err);
			}
			idlingOnCadence = polls && ok;

			if (polls) {
				try {
					transport->poll(ctx, acctCopy);
				} catch (const std::exception &e) {
					HostLog(std::string("[events] poll '") + providerId + "' crashed: " + e.what());
				} catch (...) {
					HostLog("[events] poll '" + providerId + "' crashed: unknown error");
				}
				if (Chat::CancelableSleep(std::chrono::milliseconds(transport->pollIntervalMs()),
							  canceled)) {
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
	// Authoritative terminal for this account's health (its worker's late reports are
	// now dropped by the set generation flag and cannot override this).
	Transports::Health().Report(Transports::EventsTransportId(OAuth::AccountDestination(accountId)),
				    Transports::TransportHealth::State::Disconnected);
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
		Transports::Health().Report(Transports::EventsTransportId(OAuth::AccountDestination(entry.first)),
					    Transports::TransportHealth::State::Disconnected);
	}
}

bool EventHub::Admit(NormalizedEvent &ev, const std::string &accountId)
{
	if (ev.accountId.empty()) {
		ev.accountId = accountId;
	}
	// A duplicate is not stored (Add only fills in the stored copy's ids), so it is not
	// scrubbed either.
	if (!ev.id.empty() && !Store().Contains(ev.id)) {
		removals_.Scrub(ev, Store(), TimeUtil::NowMs());
	}
	return Store().Add(ev);
}

void EventHub::Observe(const char *name, const json &payload) const
{
	if (fanoutObserver_) {
		fanoutObserver_(name, payload);
	}
}

void EventHub::SetFanoutObserver(std::function<void(const char *name, const json &payload)> observer)
{
	std::lock_guard<std::mutex> admission(admitMutex_);
	fanoutObserver_ = std::move(observer);
}

void EventHub::Ingest(const NormalizedEvent &ev)
{
	std::lock_guard<std::mutex> admission(admitMutex_);
	NormalizedEvent admitted = ev;
	if (!Admit(admitted, ev.accountId)) {
		// Mostly the expected case -- the YouTube REST transport re-emits its whole poll
		// window every tick and leans on this drop. The one worth catching is a real-time
		// event whose id backfill already seeded into the store (it seeds without
		// broadcasting), which therefore never reaches a widget.
		DBG(LogCat::Events, "ingest dropped (duplicate or no id): %s %s id=%s", ev.platform.c_str(),
		    ev.type.c_str(), ev.id.c_str());
		return; // duplicate / no id -> already emitted or unusable; drop
	}
	DBG(LogCat::Events, "ingest %s %s id=%s", ev.platform.c_str(), ev.type.c_str(), ev.id.c_str());
	json payload = admitted.ToJson();
	Observe(EventNames::kEventsNew, payload);
	AsyncTask::PostToUi([payload = std::move(payload)]() { Bridge::EmitEvent(EventNames::kEventsNew, payload); });
	// Phase 9.3: fan the same event to every open overlay widget (SSE). Called off the
	// event worker thread; Broadcast is mutex-guarded + thread-safe. Only reached for a
	// newly-stored (non-duplicate) event, so widgets never double-fire.
	Overlay::Server().Broadcast(admitted);
}

std::optional<size_t> EventHub::Replay(const std::string &id)
{
	std::lock_guard<std::mutex> admission(admitMutex_);
	const std::vector<NormalizedEvent> stored =
		Store().Select([&id](const NormalizedEvent &ev) { return ev.id == id; });
	if (stored.empty()) {
		return std::nullopt;
	}
	return Overlay::Server().Broadcast(stored.front(), /*replay=*/true);
}

namespace {

// The time two copies of one purchase can disagree by: each surface keys it to a whole second
// (YouTubeMoneyEventId), and a copy just either side of a second boundary gets another id.
constexpr int64_t kSamePurchaseSkewMs = 2000;

// Whether `other` is the copy of `paid` the Super Chat REST poll stored under another id: it
// knows no chat message id, and the same viewer paid the same amount on the same account at
// the same moment.
bool SamePurchase(const NormalizedEvent &paid, const NormalizedEvent &other)
{
	const int64_t skew = paid.ts > other.ts ? paid.ts - other.ts : other.ts - paid.ts;
	return other.id != paid.id && other.msgId.empty() && !paid.authorId.empty() &&
	       other.authorId == paid.authorId && other.accountId == paid.accountId &&
	       other.platform == paid.platform && other.type == paid.type && other.amount == paid.amount &&
	       skew <= kSamePurchaseSkewMs;
}

// Whether `redaction`, an op on `opDest`, reaches `ev`. An event stored with no broadcast (the
// account-wide Super Chat REST poll) belongs to one of its account's destinations, not known
// which, so it is held to be on `opDest`.
bool Reaches(const Chat::Redaction &redaction, const OAuth::DestinationId &opDest, const NormalizedEvent &ev)
{
	const OAuth::DestinationId on = ev.profileUuid.empty() && ev.accountId == opDest.accountId
						? opDest
						: OAuth::DestinationId{ev.accountId, ev.profileUuid};
	return redaction.Matches(OAuth::DestinationKey(on), 0, ev.ts, ev.msgId, ev.authorId);
}

} // namespace

std::vector<NormalizedEvent> RedactModeratedEvents(EventStore &store, const OAuth::DestinationId &dest,
						   const Chat::ModerationOp &op)
{
	if (op.action == Chat::ModerationAction::ClearAll) {
		return {};
	}
	Chat::ModerationOp stamped = op;
	stamped.dest = dest;
	// The chat rule itself, so an event is reached exactly when a chat line with its ids
	// would be. Seq 0 under the highest bound: the store holds only events admitted before.
	const Chat::Redaction redaction = Chat::Redaction::From(stamped, std::numeric_limits<uint64_t>::max());
	const auto reaches = [&redaction, &dest](const NormalizedEvent &ev) {
		return Reaches(redaction, dest, ev);
	};
	if (op.action != Chat::ModerationAction::Delete) {
		return store.RedactMessages(reaches, Chat::DeletedMark(op.action));
	}
	// A delete names one chat message; the REST poll's copy of the same purchase carries no
	// message id, so it is found through the copy the message id names.
	const std::vector<NormalizedEvent> named = store.Select(reaches);
	if (named.empty()) {
		return {};
	}
	return store.RedactMessages(
		[&reaches, &named](const NormalizedEvent &ev) {
			return reaches(ev) ||
			       std::any_of(named.begin(), named.end(),
					   [&ev](const NormalizedEvent &paid) { return SamePurchase(paid, ev); });
		},
		Chat::DeletedMark(op.action));
}

void RecentRemovals::Remember(const OAuth::DestinationId &dest, const Chat::ModerationOp &op, int64_t seenMs)
{
	if (op.action == Chat::ModerationAction::ClearAll) {
		return;
	}
	Chat::ModerationOp stamped = op;
	stamped.dest = dest;
	Removal removal{dest, Chat::Redaction::From(stamped, std::numeric_limits<uint64_t>::max()), seenMs};
	if (op.action == Chat::ModerationAction::ClearUser && !removal.reach.beforeTs) {
		removal.reach.beforeTs = op.happenedAt ? *op.happenedAt : seenMs + kSeenMarginMs;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	removals_.push_back(std::move(removal));
	while (removals_.size() > kMax || seenMs - removals_.front().seenMs > kKeepMs) {
		removals_.pop_front();
	}
}

bool RecentRemovals::Scrub(NormalizedEvent &ev, const EventStore &store, int64_t nowMs) const
{
	if (ev.message.empty()) {
		return false;
	}
	std::string mark;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const Removal &r : removals_) {
			if (nowMs - r.seenMs <= kKeepMs && Reaches(r.reach, r.dest, ev)) {
				mark = Chat::DeletedMark(r.reach.action);
				break;
			}
		}
	}
	// The same purchase already stored with its words removed, whenever that was: the
	// REST poll's copy of a Super Chat deleted before the poll came round.
	if (mark.empty() && ev.msgId.empty()) {
		const std::vector<NormalizedEvent> twin = store.Select(
			[&ev](const NormalizedEvent &paid) { return !paid.deleted.empty() && SamePurchase(paid, ev); });
		if (!twin.empty()) {
			mark = twin.front().deleted;
		}
	}
	if (mark.empty()) {
		return false;
	}
	ev.message.clear();
	ev.deleted = mark;
	return true;
}

void EventHub::ApplyModeration(const OAuth::DestinationId &dest, const Chat::ModerationOp &op)
{
	// Held through the fan-out below: an event admitted before this is reached by the store
	// pass and its frames went out first; one admitted after is scrubbed (admitMutex_).
	std::lock_guard<std::mutex> admission(admitMutex_);
	removals_.Remember(dest, op, TimeUtil::NowMs());
	const std::vector<NormalizedEvent> redacted = RedactModeratedEvents(Store(), dest, op);
	if (redacted.empty()) {
		return;
	}
	DBG(LogCat::Events, "moderation removed the message of %zu stored event(s)", redacted.size());
	json ids = json::array();
	json events = json::array();
	for (const NormalizedEvent &ev : redacted) {
		ids.push_back(ev.id);
		events.push_back(ev.ToJson());
	}
	Observe(EventNames::kEventsRedacted, events);
	// Ids only: a widget drops the words from the events it holds, and needs nothing else.
	Overlay::Server().BroadcastEventRedaction(json{{"ids", std::move(ids)}});
	AsyncTask::PostToUi([events = std::move(events)]() { Bridge::EmitEvent(EventNames::kEventsRedacted, events); });
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
