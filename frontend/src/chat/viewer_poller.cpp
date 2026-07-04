#include "viewer_poller.hpp"

#include <chrono>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "../async_task.hpp"
#include "../bridge.hpp"
#include "../log.hpp"
#include "../oauth/provider.hpp"
#include "../oauth/registry.hpp"
#include "../oauth/account_store.hpp"
#include "ws_client.hpp" // CancelableSleep

namespace Chat {

using json = nlohmann::json;

namespace {

// Modest poll cadence: frequent enough to feel live, light on platform quota
// (YouTube videos.list + Twitch/Kick Get Streams each cost little, but the YouTube
// chat poll already consumes quota concurrently).
constexpr std::chrono::milliseconds kPollInterval(20000);

} // namespace

void ViewerPoller::Start()
{
	// Idempotent: tear down any prior generation before arming a fresh one so a
	// re-Start never doubles workers.
	Stop();

	auto stop = std::make_shared<std::atomic<bool>>(false);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = stop;
	}

	// The worker owns only the generation cancel flag (by shared_ptr). It reads the
	// token store + registry singletons (alive to process exit) and never touches CEF
	// except through the alive-guarded PostToUi, so it is safe even if it outlives a
	// Stop() (it is detached, not joined).
	AsyncTask::RunAsync([stop]() {
		auto canceled = [stop] { return stop->load(std::memory_order_acquire); };

		while (!canceled()) {
			json perAccount = json::object();
			long long total = 0;

			// Re-read accounts each cycle so a connect/disconnect mid-stream is picked
			// up. viewerCount's SendAuthed/ensureFresh is store-coherent, so polling on
			// a by-value copy is safe.
			for (const auto &entry : OAuth::Accounts().All()) {
				if (canceled()) {
					break;
				}
				OAuth::OAuthAccount acct = entry.second;
				OAuth::StreamProvider *provider = OAuth::Registry().Get(acct.providerId);
				if (!provider || !provider->isTokenScopeCurrent(acct)) {
					continue;
				}

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
					// false = unsupported / not live / errored -> omit from the
					// aggregate. Log only a real error so a slow back-off isn't needed
					// (the 20s cadence already paces retries); never abort the cycle.
					if (!err.empty()) {
						HostLog("[viewers] '" + acct.providerId + "' skipped: " + err);
					}
					continue;
				}
				if (count < 0) {
					count = 0;
				}

				// Aggregate per accountId (each connected account counted once;
				// two accounts on one platform are distinct rows).
				perAccount[OAuth::AccountId(acct)] = count;
				total += count;
			}

			if (canceled()) {
				break;
			}

			json payload = json{{"perAccount", std::move(perAccount)}, {"total", total}};
			AsyncTask::PostToUi(
				[payload = std::move(payload)] { Bridge::EmitEvent("viewers.changed", payload); });

			// Sliced wait so a Stop() between cycles is honored within ~0.5s rather
			// than blocking the full interval.
			if (CancelableSleep(kPollInterval, canceled)) {
				break;
			}
		}
	});

	HostLog("[viewers] poller started");
}

void ViewerPoller::Stop()
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

ViewerPoller &Viewers()
{
	static ViewerPoller poller;
	return poller;
}

} // namespace Chat
