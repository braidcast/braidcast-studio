#include "viewer_poller.hpp"
#include "../event_names.hpp"

#include <chrono>
#include <map>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "../log.hpp"
#include "../oauth/provider.hpp"
#include "util/op_error.hpp"

namespace Chat {

namespace {

// Modest poll cadence: frequent enough to feel live, light on every platform's budget. Not
// tightened now that YouTube's read costs no quota -- the logged-out watch page polls its own
// viewer figure every 5s, so 20s stays 4x more conservative than an ordinary browser tab, and
// that traffic shape is the constraint here rather than a unit price.
constexpr std::chrono::milliseconds kPollInterval(20000);

} // namespace

const char *ViewerPoller::LogTag() const
{
	return "viewers";
}

const char *ViewerPoller::EventName() const
{
	return EventNames::kViewersChanged;
}

std::chrono::milliseconds ViewerPoller::Interval(unsigned long long) const
{
	return kPollInterval;
}

void ViewerPoller::PollAccount(OAuth::OAuthAccount &acct, OAuth::StreamProvider *provider, PollCycle &cycle)
{
	std::map<OAuth::DestinationId, int> counts;
	std::string err;
	bool ok = false;
	try {
		// Per DESTINATION, not per account: an account with several concurrent broadcasts
		// contributes each of them. For a platform with one channel per account the default
		// implementation is still exactly one call reported under the account-wide
		// destination, so nothing about Twitch/Kick's cost or shape changes.
		ok = provider->viewerCounts(acct, counts, err);
	} catch (const std::exception &e) {
		ok = false;
		err = std::string("viewer count crashed: ") + e.what();
	} catch (...) {
		ok = false;
		err = "viewer count crashed: unknown error";
	}
	if (!ok) {
		// false = unsupported / not live / errored -> omit from the aggregate. Log
		// only a real error so a slow back-off isn't needed (the 20s cadence already
		// paces retries); never abort the cycle.
		if (!err.empty()) {
			HostLog("[viewers] '" + acct.providerId + "' skipped: " + Err::Diagnostic(err));
		}
		return;
	}
	// A partial read (some of the account's broadcasts read, others failed) still reports
	// the rows that succeeded -- a partial total beats no total -- but must still surface
	// why the rest are missing.
	if (!err.empty()) {
		HostLog("[viewers] '" + acct.providerId + "' partial: " + Err::Diagnostic(err));
	}

	long long accountTotal = 0;
	for (const auto &entry : counts) {
		const int count = entry.second < 0 ? 0 : entry.second;
		accountTotal += count;
		cycle.rows.push_back(json{{"key", OAuth::DestinationKey(entry.first)},
					  {"accountId", entry.first.accountId},
					  {"profileUuid", entry.first.profileUuid},
					  {"count", count}});
	}

	// perAccount keeps its existing meaning and key (accountId -> that account's viewers),
	// now the SUM over the account's live broadcasts rather than whichever single broadcast
	// the cache happened to hold. Existing consumers index it by accountId unchanged.
	cycle.perAccount[OAuth::AccountId(acct)] = accountTotal;
}

std::optional<json> ViewerPoller::BuildPayload(PollCycle &&cycle)
{
	long long total = 0;
	for (const auto &entry : cycle.perAccount.items()) {
		total += entry.value().get<long long>();
	}
	// `perDestination` is additive detail: the same numbers broken out per broadcast, for a
	// consumer that wants to label each orientation. `total`/`perAccount` stay authoritative
	// and unchanged in shape, so nothing reading them needs to know destinations exist.
	return json{{"perAccount", std::move(cycle.perAccount)},
		    {"total", total},
		    {"perDestination", std::move(cycle.rows)}};
}

ViewerPoller &Viewers()
{
	static ViewerPoller poller;
	return poller;
}

} // namespace Chat
