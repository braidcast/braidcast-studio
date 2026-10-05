#ifndef OBS_MULTISTREAM_FRONTEND_FX_FX_RATES_HPP_
#define OBS_MULTISTREAM_FRONTEND_FX_FX_RATES_HPP_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "fx_parse.hpp"

// Exchange rates for showing a Super Chat in the streamer's own currency (roadmap 9.6). The
// app fetches the European Central Bank's daily reference rates itself -- there is no
// Braidcast server -- keeps the last good table on disk (fx_rates.json), and refreshes it in
// the background. A figure converted with them is approximate: YouTube's own payout differs
// after fees and its own conversion, and the reference rate is a day's midpoint.
//
// What every consumer reads is Snapshot(): the bridge's fx.get and fx.changed, and each
// overlay page's bootstrap. The conversion itself is done where the figure is drawn
// (web/src/lib/utils/fx.ts), with the stale and missing cases decided there from this.
namespace Fx {

using json = nlohmann::json;

// How old the ECB's reference date may be before conversions stop. The ECB publishes on
// working days only, so a long holiday weekend is four days; past a week something is wrong.
inline constexpr int64_t kStaleAfterDays = 7;

class RateStore {
public:
	// Load the cached table, take the home-currency setting ("" = automatic) and start the
	// refresh worker. Idempotent: a second Start replaces the worker.
	void Start(const std::string &homeSetting);
	// Signal the worker to stop. Signal-only, like the account pollers: it is detached and
	// unwinds within about half a second, or when an in-flight fetch times out.
	void Stop();

	// The home-currency setting changed ("" = automatic). Announces fx.changed.
	void SetHomeSetting(const std::string &homeSetting);

	// { home, homeSource, date, fetchedAt, stale, rates: { EUR: 1, USD: ..., ... } }.
	// `home` is "" when nothing names a currency the rates cover; `rates` is empty before
	// any table was fetched. Safe from any thread.
	json Snapshot() const;

	// The snapshot for an explicit table and home, at `todayDays` (days since 1970-01-01).
	// What Snapshot() returns, without the store's state, for the self-test.
	static json SnapshotOf(const RateTable &table, int64_t fetchedAtMs, int64_t todayDays, const HomeChoice &home);

private:
	void Run(const std::shared_ptr<std::atomic<bool>> &stop);
	bool Fetch();
	void LoadCache();
	void SaveCache() const;
	HomeChoice Home() const;

	mutable std::mutex mutex_;
	RateTable table_;
	int64_t fetchedAtMs_ = 0;
	std::string homeSetting_;
	std::string localeRegion_;
	std::shared_ptr<std::atomic<bool>> stop_;
};

RateStore &Rates();

} // namespace Fx

#endif // OBS_MULTISTREAM_FRONTEND_FX_FX_RATES_HPP_
