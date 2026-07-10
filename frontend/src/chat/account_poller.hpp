#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_ACCOUNT_POLLER_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_ACCOUNT_POLLER_HPP_

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

#include <nlohmann/json.hpp>

#include "../oauth/provider.hpp" // OAuthAccount, StreamProvider

namespace Chat {

using json = nlohmann::json;

// Shared skeleton for the account pollers (ViewerPoller, ChannelStatsPoller). Owns
// the idempotent Start()/Stop() lifecycle and the per-cycle scaffold: re-read every
// account, skip the unconnected, poll each connected one, then emit through the
// alive-guarded PostToUi + Bridge::EmitEvent path and wait out a cancelable sleep.
// Subclasses supply only the parts that differ via the pure-virtual hooks below.
//
// The worker is detached; Stop() signals its cancel flag and the worker exits within
// ~0.5s (a sliced CancelableSleep + a canceled() check between platform calls). Start
// is idempotent (calls Stop() first) so a re-Start never doubles workers. A late emit
// after Shutdown is dropped by the alive-guard rather than touching CEF. The worker
// reads the account store + registry singletons and its own poller singleton, all
// alive to process exit, so it is safe even if it outlives a Stop().
class AccountPoller {
public:
	virtual ~AccountPoller() = default;

	// Spawn the single poll worker. Idempotent: calls Stop() first so a re-Start
	// never leaves a stale worker running.
	void Start();

	// Signal the worker to stop. Idempotent; safe when nothing is running.
	void Stop();

protected:
	// Log tag (e.g. "viewers") for the "[<tag>] poller started" line.
	virtual const char *LogTag() const = 0;

	// Bridge event emitted each cycle (e.g. "viewers.changed").
	virtual const char *EventName() const = 0;

	// Wait between cycles. `tick` is a rolling per-cycle counter (from 0) a subclass
	// can fold deterministic jitter into; ignore it for a fixed cadence.
	virtual std::chrono::milliseconds Interval(unsigned long long tick) const = 0;

	// Poll one connected account: fill perAccount[AccountId(acct)] and do any side
	// effects (persist, cached fallback). `provider` is the registered StreamProvider
	// for acct.providerId.
	virtual void PollAccount(OAuth::OAuthAccount &acct, OAuth::StreamProvider *provider, json &perAccount) = 0;

	// Assemble the emit payload from the accumulated perAccount map. Return
	// std::nullopt to skip the emit for this cycle.
	virtual std::optional<json> BuildPayload(json &&perAccount) = 0;

private:
	void RunPollLoop(const std::shared_ptr<std::atomic<bool>> &stop);

	std::mutex mutex_;
	std::shared_ptr<std::atomic<bool>> stop_; // current generation's cancel flag
};

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_ACCOUNT_POLLER_HPP_
