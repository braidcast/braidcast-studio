#include "account_poller.hpp"

#include <string>
#include <utility>

#include "../async_task.hpp"
#include "../bridge.hpp"
#include "../log.hpp"
#include "../oauth/registry.hpp"
#include "../oauth/account_store.hpp"
#include "ws_client.hpp" // CancelableSleep

namespace Chat {

void AccountPoller::Start()
{
	// Idempotent: tear down any prior generation before arming a fresh one so a
	// re-Start never doubles workers.
	Stop();

	auto stop = std::make_shared<std::atomic<bool>>(false);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = stop;
	}

	// The worker owns the generation cancel flag (by shared_ptr) and dispatches back
	// into this poller singleton (alive to process exit) for the per-cycle hooks. It
	// reads the account store + registry singletons and never touches CEF except
	// through the alive-guarded PostToUi, so it is safe even if it outlives a Stop()
	// (it is detached, not joined).
	AsyncTask::RunAsync([this, stop]() { RunPollLoop(stop); });

	HostLog(std::string("[") + LogTag() + "] poller started");
}

void AccountPoller::Stop()
{
	std::shared_ptr<std::atomic<bool>> stop;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop = stop_;
		stop_.reset();
	}
	if (stop) {
		stop->store(true, std::memory_order_release); // signal this generation's loop
	}
}

void AccountPoller::RunPollLoop(const std::shared_ptr<std::atomic<bool>> &stop)
{
	auto canceled = [stop] {
		return stop->load(std::memory_order_acquire);
	};

	unsigned long long tick = 0;
	while (!canceled()) {
		json perAccount = json::object();

		// Re-read accounts each cycle so a connect/disconnect between ticks is picked
		// up. The provider read's SendAuthed/ensureFresh is store-coherent, so polling
		// on a by-value copy is safe.
		for (const auto &entry : OAuth::Accounts().All()) {
			if (canceled()) {
				break;
			}
			OAuth::OAuthAccount acct = entry.second;
			OAuth::StreamProvider *provider = OAuth::Registry().Get(acct.providerId);
			if (!OAuth::IsAccountConnected(acct)) {
				continue;
			}
			PollAccount(acct, provider, perAccount);
		}

		if (canceled()) {
			break;
		}

		if (std::optional<json> payload = BuildPayload(std::move(perAccount))) {
			const char *event = EventName();
			AsyncTask::PostToUi(
				[event, payload = std::move(*payload)] { Bridge::EmitEvent(event, payload); });
		}

		// Sliced wait so a Stop() between cycles is honored within ~0.5s rather than
		// blocking the full interval.
		if (CancelableSleep(Interval(tick), canceled)) {
			break;
		}
		++tick;
	}
}

} // namespace Chat
