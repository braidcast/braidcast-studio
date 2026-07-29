#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHANNEL_STATS_POLLER_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHANNEL_STATS_POLLER_HPP_

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "account_poller.hpp"

// Always-on audience-total poller (Channel identity feature). Unlike ViewerPoller
// it is NOT gated on streaming: Start() at bootstrap, Stop() at Bridge::Shutdown.
// Each tick (~15 min, jittered) it reads every connected, scope-current account's
// StreamProvider::audienceCount, persists a changed total onto the account record,
// and emits `channels.stats` -- though a tick only BUYS a fresh read on the live/idle
// cadence in the .cpp; otherwise it re-emits the last-known value with its as-of
// stamp, which costs nothing. Providers with no REST total (Kick) return
// available=false and are skipped (their number arrives live via kick_events).
// That one payload object also goes to the overlay server's SSE clients as the
// named `channels` event, so an overlay widget and the panel can never show
// different totals.
// Emits go through the alive-guarded PostToUi + Bridge::EmitEvent path. See
// AccountPoller for the shared idempotent-Start / detached-worker contract.
namespace Chat {

class ChannelStatsPoller : public AccountPoller {
protected:
	const char *LogTag() const override;
	const char *EventName() const override;
	std::chrono::milliseconds Interval(unsigned long long tick) const override;
	void PollAccount(OAuth::OAuthAccount &acct, OAuth::StreamProvider *provider, PollCycle &cycle) override;
	std::optional<json> BuildPayload(PollCycle &&cycle) override;

private:
	// Spend a platform request on `accountId` this cycle? The CYCLE is unchanged (every tick
	// still emits, so a freshly-opened dock or a browser source that just connected gets the
	// last-known total with its as-of stamp at once); this decides only whether the tick pays
	// for a NEW one. Stamps the account on a yes, so the answer is the same for every caller
	// in a cycle and one account cannot consume another's slot.
	bool ShouldRead(const std::string &accountId);

	// Guarded rather than plain: Stop() only signals its detached worker, so a stop-then-start
	// can briefly leave two generations calling this at once (see PollCycle).
	std::mutex readMutex_;
	std::map<std::string, std::chrono::steady_clock::time_point> lastRead_;
};

ChannelStatsPoller &Channels();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_CHANNEL_STATS_POLLER_HPP_
