#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHANNEL_STATS_POLLER_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHANNEL_STATS_POLLER_HPP_

#include <chrono>
#include <optional>

#include "account_poller.hpp"

// Always-on audience-total poller (Channel identity feature). Unlike ViewerPoller
// it is NOT gated on streaming: Start() at bootstrap, Stop() at Bridge::Shutdown.
// Each tick (~90s, jittered) it reads every connected, scope-current account's
// StreamProvider::audienceCount, persists a changed total onto the account record,
// and emits `channels.stats`. Providers with no REST total (Kick) return
// available=false and are skipped (their number arrives live via kick_events).
// Emits go through the alive-guarded PostToUi + Bridge::EmitEvent path. See
// AccountPoller for the shared idempotent-Start / detached-worker contract.
namespace Chat {

class ChannelStatsPoller : public AccountPoller {
protected:
	const char *LogTag() const override;
	const char *EventName() const override;
	std::chrono::milliseconds Interval(unsigned long long tick) const override;
	void PollAccount(OAuth::OAuthAccount &acct, OAuth::StreamProvider *provider, json &perAccount) override;
	std::optional<json> BuildPayload(json &&perAccount) override;
};

ChannelStatsPoller &Channels();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_CHANNEL_STATS_POLLER_HPP_
