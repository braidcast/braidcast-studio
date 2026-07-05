#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHANNEL_STATS_POLLER_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHANNEL_STATS_POLLER_HPP_

#include <atomic>
#include <memory>
#include <mutex>

// Always-on audience-total poller (Channel identity feature). Unlike ViewerPoller
// it is NOT gated on streaming: Start() at bootstrap, Stop() at Bridge::Shutdown.
// Each tick (~90s, jittered) it reads every connected, scope-current account's
// StreamProvider::audienceCount, persists a changed total onto the account record,
// and emits `channels.stats`. Providers with no REST total (Kick) return
// available=false and are skipped (their number arrives live via kick_events).
// Emits go through the alive-guarded PostToUi + Bridge::EmitEvent path.
namespace Chat {

class ChannelStatsPoller {
public:
	void Start();
	void Stop();

private:
	std::mutex mutex_;
	std::shared_ptr<std::atomic<bool>> stop_;
};

ChannelStatsPoller &Channels();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_CHANNEL_STATS_POLLER_HPP_
