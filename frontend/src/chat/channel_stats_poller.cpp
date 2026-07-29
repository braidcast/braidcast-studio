#include "channel_stats_poller.hpp"
#include "../event_names.hpp"

#include <chrono>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <util/platform.h> // os_gettime_ns

#include "../log.hpp"
#include "../obs_bootstrap.hpp" // ObsBootstrap::AnyOutputLive -- the live/idle read cadence
#include "../oauth/provider.hpp"
#include "../oauth/account_store.hpp"
#include "../overlay/overlay_server.hpp" // OverlayServer::BroadcastChannelStats
#include "../overlay/overlay_store.hpp"  // Overlay::Server()
#include "util/op_error.hpp"

namespace Chat {

namespace {

// Always-on cadence: a slow base plus a small deterministic jitter so many
// installs don't align their audience reads into a synchronized burst against the
// platforms. The jitter is derived from a rolling tick counter (not RNG / clock)
// so it is reproducible and adds no nondeterminism to teardown timing.
//
// The base is slow regardless of whether anything is live. A follower/subscriber
// total moves by a handful over a whole broadcast, so polling it faster buys no
// visible freshness, while this poller runs boot-to-exit per account and each read
// costs a YouTube quota unit against a 10,000/day project budget. The panel renders
// the persisted last-known value with an as-of stamp, so a stale total reads as
// stale rather than as wrong.
constexpr std::chrono::milliseconds kBaseInterval(900000);

// How stale a total may get before a tick is allowed to BUY a fresh one. The cycle above is
// unchanged -- every tick still emits, so a dock that just opened and a browser source that
// just connected both get the last-known value with its as-of stamp immediately -- and these
// decide only which ticks pay a platform request for a new one.
//
// The split exists because this poller runs boot-to-exit and YouTube's read costs a unit
// against a 10,000/day budget shared by every install: 4 units/hour/account, forever, whether
// or not anything is on air. While live the old cadence stands (an overlay follower counter in
// a live scene is a real consumer and should tick). Idle it drops to a twelfth of that: a
// subscriber total moves by a handful over a whole day, the panel labels its value with an
// as-of stamp, and going live re-reads on the very next tick because the shorter live gap is
// then already satisfied.
//
// Slightly under the tick so the jitter folded into it can never push an elapsed live cycle
// just short of the gap and silently halve the live cadence.
constexpr std::chrono::milliseconds kLiveReadGap(870000);
constexpr std::chrono::milliseconds kIdleReadGap(7200000);

} // namespace

const char *ChannelStatsPoller::LogTag() const
{
	return "channels";
}

const char *ChannelStatsPoller::EventName() const
{
	return EventNames::kChannelsStats;
}

std::chrono::milliseconds ChannelStatsPoller::Interval(unsigned long long tick) const
{
	// Deterministic per-tick jitter (0..15000 ms) folded onto the base interval;
	// derived from the rolling tick counter, not RNG / wall-clock.
	std::chrono::milliseconds jitter((tick * 7919) % 15000);
	return kBaseInterval + jitter;
}

bool ChannelStatsPoller::ShouldRead(const std::string &accountId)
{
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	const std::chrono::milliseconds gap = ObsBootstrap::AnyOutputLive() ? kLiveReadGap : kIdleReadGap;

	const std::lock_guard<std::mutex> guard(readMutex_);
	auto it = lastRead_.find(accountId);
	if (it != lastRead_.end() && now - it->second < gap) {
		return false;
	}
	// Stamped on the ATTEMPT, not on success: a failing read must be paced like a
	// succeeding one, or a platform outage turns into a request every cycle.
	lastRead_[accountId] = now;
	return true;
}

void ChannelStatsPoller::PollAccount(OAuth::OAuthAccount &acct, OAuth::StreamProvider *provider, PollCycle &cycle)
{
	OAuth::AudienceResult out;
	std::string err;
	bool ok = false;
	if (ShouldRead(OAuth::AccountId(acct))) {
		try {
			ok = provider->audienceCount(acct, out, err);
		} catch (const std::exception &e) {
			ok = false;
			err = std::string("audience count crashed: ") + e.what();
		} catch (...) {
			ok = false;
			err = "audience count crashed: unknown error";
		}
		if (!ok && !err.empty()) {
			// A real error (not merely unsupported) -> log, but still fall through to the
			// cached-fallback below so the panel keeps its last-known value rather than
			// blanking on a transient failure. The slow cadence already paces retries, so
			// never abort the cycle.
			HostLog("[channels] '" + acct.providerId + "' skipped: " + Err::Diagnostic(err));
		}
	}

	if (ok && out.available) {
		// Persist only on a real change so a steady total doesn't churn the DPAPI blob
		// every 90s.
		if (out.count != acct.audienceCount || out.kind != acct.audienceKind ||
		    out.hidden != acct.audienceHidden) {
			acct.audienceCount = out.count;
			acct.audienceKind = out.kind;
			acct.audienceHidden = out.hidden;
			// Monotonic since boot, not wall-clock: this is an opaque change-marker
			// only. It is persisted, so it must NOT be diffed against a fresh
			// os_gettime_ns() across a restart (e.g. an "updated N ago" label) --
			// switch to an epoch clock first if ever rendered.
			acct.audienceUpdatedNs = (int64_t)os_gettime_ns();
			// Field-scoped persist: never round-trips access/refresh, so a concurrent
			// token refresh on this account isn't clobbered by our stale copy (and a
			// mid-poll removal isn't resurrected).
			OAuth::Accounts().UpdateAudience(OAuth::AccountId(acct), out.count, out.kind, out.hidden,
							 acct.audienceUpdatedNs);
		}

		// Include a fresh read every tick (even unchanged) so a freshly-loaded UI / a
		// new CEF browser always receives current values.
		cycle.perAccount[OAuth::AccountId(acct)] = json{
			{"audienceCount", out.count},
			{"audienceKind", OAuth::AudienceKindName(out.kind)},
			{"audienceHidden", out.hidden},
			{"audienceUpdatedNs", acct.audienceUpdatedNs},
		};
	} else if (acct.audienceCount >= 0) {
		// No live read this tick (Kick has no REST total, or the read failed), but a
		// persisted last-known value exists -> emit the CACHED record so the panel
		// shows "last-known + as-of" off-stream instead of "—". No persist and no
		// store write: nothing changed.
		cycle.perAccount[OAuth::AccountId(acct)] = json{
			{"audienceCount", acct.audienceCount},
			{"audienceKind", OAuth::AudienceKindName(acct.audienceKind)},
			{"audienceHidden", acct.audienceHidden},
			{"audienceUpdatedNs", acct.audienceUpdatedNs},
		};
	}
}

std::optional<json> ChannelStatsPoller::BuildPayload(PollCycle &&cycle)
{
	if (cycle.perAccount.empty()) {
		return std::nullopt;
	}
	json payload = json{{"perAccount", std::move(cycle.perAccount)}};

	// Overlay widgets read the SAME payload object the bridge emit carries, so a widget's
	// total can never disagree with the panel's. Fanned out HERE on the poll worker rather
	// than after the UI hop (mirrors the viewer poller): BroadcastChannelStats does blocking
	// socket sends bounded by the overlay server's send timeout, and every browser source
	// renders on the frontend's TID_UI, so one stalled overlay reader would freeze them all.
	// dump() can throw on a malformed payload (invalid UTF-8 in a platform-supplied id):
	// skip the fan-out and still forward to the (guarded) bridge.
	try {
		Overlay::Server().BroadcastChannelStats(payload);
	} catch (...) {
		// malformed audience payload -> skip the overlay fan-out
	}
	return payload;
}

ChannelStatsPoller &Channels()
{
	static ChannelStatsPoller poller;
	return poller;
}

} // namespace Chat
