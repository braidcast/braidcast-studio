#include "channel_stats_poller.hpp"

#include <chrono>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <util/platform.h> // os_gettime_ns

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

// Always-on cadence: a slow base plus a small deterministic jitter so many
// installs don't align their audience reads into a synchronized burst against the
// platforms. The jitter is derived from a rolling tick counter (not RNG / clock)
// so it is reproducible and adds no nondeterminism to teardown timing.
constexpr std::chrono::milliseconds kBaseInterval(90000);

} // namespace

void ChannelStatsPoller::Start()
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
	// account store + registry singletons (alive to process exit) and never touches CEF
	// except through the alive-guarded PostToUi, so it is safe even if it outlives a
	// Stop() (it is detached, not joined).
	AsyncTask::RunAsync([stop]() {
		auto canceled = [stop] { return stop->load(std::memory_order_acquire); };

		unsigned long long tick = 0;
		while (!canceled()) {
			json perAccount = json::object();

			// Re-read accounts each cycle so a connect/disconnect between ticks is
			// picked up. audienceCount's SendAuthed/ensureFresh is store-coherent, so
			// polling on a by-value copy is safe; a changed total is written back via
			// Put below.
			for (const auto &entry : OAuth::Accounts().All()) {
				if (canceled()) {
					break;
				}
				OAuth::OAuthAccount acct = entry.second;
				OAuth::StreamProvider *provider = OAuth::Registry().Get(acct.providerId);
				if (!provider || !provider->isTokenScopeCurrent(acct)) {
					continue;
				}

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
				if (!ok) {
					// false = unsupported / not obtainable / errored -> leave the cached
					// count untouched. Log only a real error; the slow cadence already
					// paces retries, so never abort the cycle.
					if (!err.empty()) {
						HostLog("[channels] '" + acct.providerId + "' skipped: " + err);
					}
					continue;
				}
				if (!out.available) {
					// No REST total (Kick); its number arrives live via kick_events.
					continue;
				}

				// Persist only on a real change so a steady total doesn't churn the
				// DPAPI blob every 90s.
				if (out.count != acct.audienceCount || out.kind != acct.audienceKind ||
				    out.hidden != acct.audienceHidden) {
					acct.audienceCount = out.count;
					acct.audienceKind = out.kind;
					acct.audienceHidden = out.hidden;
					acct.audienceUpdatedNs = (int64_t)os_gettime_ns();
					OAuth::Accounts().Put(OAuth::AccountId(acct), acct);
				}

				// Always include an available read in the emit (even unchanged) so a
				// freshly-loaded UI / a new CEF browser always receives current values.
				perAccount[OAuth::AccountId(acct)] = json{
					{"audienceCount", out.count},
					{"audienceKind", OAuth::AudienceKindName(out.kind)},
					{"audienceHidden", out.hidden},
					{"audienceUpdatedNs", acct.audienceUpdatedNs},
				};
			}

			if (canceled()) {
				break;
			}

			if (!perAccount.empty()) {
				json payload = json{{"perAccount", std::move(perAccount)}};
				AsyncTask::PostToUi(
					[payload = std::move(payload)] { Bridge::EmitEvent("channels.stats", payload); });
			}

			// Deterministic per-tick jitter (0..15000 ms) folded onto the base interval;
			// derived from the rolling tick counter, not RNG / wall-clock.
			std::chrono::milliseconds jitter((tick * 7919) % 15000);
			++tick;

			// Sliced wait so a Stop() between cycles is honored within ~0.5s rather
			// than blocking the full interval.
			if (CancelableSleep(kBaseInterval + jitter, canceled)) {
				break;
			}
		}
	});

	HostLog("[channels] poller started");
}

void ChannelStatsPoller::Stop()
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

ChannelStatsPoller &Channels()
{
	static ChannelStatsPoller poller;
	return poller;
}

} // namespace Chat
