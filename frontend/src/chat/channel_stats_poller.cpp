#include "channel_stats_poller.hpp"

#include <chrono>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <util/platform.h> // os_gettime_ns

#include "../log.hpp"
#include "../oauth/provider.hpp"
#include "../oauth/account_store.hpp"

namespace Chat {

namespace {

// Always-on cadence: a slow base plus a small deterministic jitter so many
// installs don't align their audience reads into a synchronized burst against the
// platforms. The jitter is derived from a rolling tick counter (not RNG / clock)
// so it is reproducible and adds no nondeterminism to teardown timing.
constexpr std::chrono::milliseconds kBaseInterval(90000);

} // namespace

const char *ChannelStatsPoller::LogTag() const
{
	return "channels";
}

const char *ChannelStatsPoller::EventName() const
{
	return "channels.stats";
}

std::chrono::milliseconds ChannelStatsPoller::Interval(unsigned long long tick) const
{
	// Deterministic per-tick jitter (0..15000 ms) folded onto the base interval;
	// derived from the rolling tick counter, not RNG / wall-clock.
	std::chrono::milliseconds jitter((tick * 7919) % 15000);
	return kBaseInterval + jitter;
}

void ChannelStatsPoller::PollAccount(OAuth::OAuthAccount &acct, OAuth::StreamProvider *provider, json &perAccount)
{
	OAuth::AudienceResult out;
	std::string err;
	bool ok = false;
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
		HostLog("[channels] '" + acct.providerId + "' skipped: " + err);
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
		perAccount[OAuth::AccountId(acct)] = json{
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
		perAccount[OAuth::AccountId(acct)] = json{
			{"audienceCount", acct.audienceCount},
			{"audienceKind", OAuth::AudienceKindName(acct.audienceKind)},
			{"audienceHidden", acct.audienceHidden},
			{"audienceUpdatedNs", acct.audienceUpdatedNs},
		};
	}
}

std::optional<json> ChannelStatsPoller::BuildPayload(json &&perAccount)
{
	if (perAccount.empty()) {
		return std::nullopt;
	}
	return json{{"perAccount", std::move(perAccount)}};
}

ChannelStatsPoller &Channels()
{
	static ChannelStatsPoller poller;
	return poller;
}

} // namespace Chat
