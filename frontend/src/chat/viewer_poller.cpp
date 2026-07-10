#include "viewer_poller.hpp"

#include <chrono>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "../log.hpp"
#include "../oauth/provider.hpp"

namespace Chat {

namespace {

// Modest poll cadence: frequent enough to feel live, light on platform quota
// (YouTube videos.list + Twitch/Kick Get Streams each cost little, but the YouTube
// chat poll already consumes quota concurrently).
constexpr std::chrono::milliseconds kPollInterval(20000);

} // namespace

const char *ViewerPoller::LogTag() const
{
	return "viewers";
}

const char *ViewerPoller::EventName() const
{
	return "viewers.changed";
}

std::chrono::milliseconds ViewerPoller::Interval(unsigned long long) const
{
	return kPollInterval;
}

void ViewerPoller::PollAccount(OAuth::OAuthAccount &acct, OAuth::StreamProvider *provider, json &perAccount)
{
	int count = 0;
	std::string err;
	bool ok = false;
	try {
		ok = provider->viewerCount(acct, count, err);
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
			HostLog("[viewers] '" + acct.providerId + "' skipped: " + err);
		}
		return;
	}
	if (count < 0) {
		count = 0;
	}

	// Aggregate per accountId (each connected account counted once; two accounts on
	// one platform are distinct rows). The cycle total is summed in BuildPayload.
	perAccount[OAuth::AccountId(acct)] = count;
}

std::optional<json> ViewerPoller::BuildPayload(json &&perAccount)
{
	long long total = 0;
	for (const auto &entry : perAccount.items()) {
		total += entry.value().get<long long>();
	}
	return json{{"perAccount", std::move(perAccount)}, {"total", total}};
}

ViewerPoller &Viewers()
{
	static ViewerPoller poller;
	return poller;
}

} // namespace Chat
