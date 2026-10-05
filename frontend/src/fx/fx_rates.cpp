#include "fx_rates.hpp"

#include <chrono>
#include <vector>

#include "../bridge.hpp"
#include "../chat/ws_client.hpp" // Chat::CancelableSleep
#include "../event_names.hpp"
#include "../log.hpp"
#include "../multistream/StorePaths.hpp"
#include "../oauth/account_store.hpp"
#include "../util/async_task.hpp"
#include "../util/file_util.hpp"
#include "../util/http_client.hpp"
#include "../util/text_encoding.hpp"
#include "../util/time_util.hpp"
#include "../util/user_locale.hpp"

namespace Fx {

namespace {

constexpr char kEcbDailyUrl[] = "https://www.ecb.europa.eu/stats/eurofxref/eurofxref-daily.xml";
constexpr char kCacheFile[] = "fx_rates.json";

// A table older than this is fetched again. The ECB publishes once a working day, around
// 16:00 CET, so twice a day catches each publication within half a day.
constexpr auto kRefreshAfter = std::chrono::hours(12);
// How often the worker wakes to ask whether a refresh is due.
constexpr auto kCheckEvery = std::chrono::hours(1);
// After a failed fetch, the next try. The cached table, if any, keeps serving meanwhile.
constexpr auto kRetryAfter = std::chrono::minutes(30);
constexpr int kFetchTimeoutSec = 20;

void Announce(json snapshot)
{
	AsyncTask::PostToUi([snapshot = std::move(snapshot)] { Bridge::EmitEvent(EventNames::kFxChanged, snapshot); });
}

} // namespace

json RateStore::SnapshotOf(const RateTable &table, int64_t fetchedAtMs, int64_t todayDays, const HomeChoice &home)
{
	json rates = json::object();
	if (!table.perEur.empty()) {
		rates["EUR"] = 1.0;
		for (const auto &[code, perEur] : table.perEur) {
			rates[code] = perEur;
		}
	}
	const int64_t day = DaysFromIsoDate(table.date);
	const bool stale = day < 0 || todayDays - day > kStaleAfterDays;
	return json{{"home", home.code}, {"homeSource", home.source}, {"date", table.date}, {"fetchedAt", fetchedAtMs},
		    {"stale", stale},    {"rates", std::move(rates)}};
}

HomeChoice RateStore::Home() const
{
	std::string setting;
	std::string localeRegion;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		setting = homeSetting_;
		localeRegion = localeRegion_;
	}
	// Read on each call rather than cached: a YouTube channel connected after start names a
	// country the store has to pick up, and the account store is safe from any thread.
	std::vector<std::string> countries;
	for (const auto &entry : OAuth::Accounts().All()) {
		if (entry.second.providerId == "youtube" && !entry.second.country.empty()) {
			countries.push_back(entry.second.country);
		}
	}
	return ResolveHome(setting, countries, localeRegion);
}

json RateStore::Snapshot() const
{
	const HomeChoice home = Home();
	std::lock_guard<std::mutex> lock(mutex_);
	return SnapshotOf(table_, fetchedAtMs_, TimeUtil::NowMs() / TimeUtil::kDayMs, home);
}

void RateStore::Start(const std::string &homeSetting)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		homeSetting_ = homeSetting;
		const std::wstring locale = UserLocale::NameW();
		localeRegion_ = RegionOfLocale(Encoding::WideToUtf8(locale.c_str()));
	}
	LoadCache();
	Stop();
	auto stop = std::make_shared<std::atomic<bool>>(false);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = stop;
	}
	AsyncTask::RunAsync([this, stop] { Run(stop); });
}

void RateStore::Stop()
{
	std::shared_ptr<std::atomic<bool>> stop;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop = std::move(stop_);
	}
	if (stop) {
		stop->store(true, std::memory_order_release);
	}
}

void RateStore::SetHomeSetting(const std::string &homeSetting)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (homeSetting_ == homeSetting) {
			return;
		}
		homeSetting_ = homeSetting;
	}
	Bridge::EmitEvent(EventNames::kFxChanged, Snapshot());
}

void RateStore::Run(const std::shared_ptr<std::atomic<bool>> &stop)
{
	const auto canceled = [stop] {
		return stop->load(std::memory_order_acquire);
	};
	while (!canceled()) {
		int64_t fetchedAt = 0;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			fetchedAt = table_.perEur.empty() ? 0 : fetchedAtMs_;
		}
		const int64_t ageMs = TimeUtil::NowMs() - fetchedAt;
		std::chrono::milliseconds wait = kCheckEvery;
		if (ageMs < 0 ||
		    ageMs >= std::chrono::duration_cast<std::chrono::milliseconds>(kRefreshAfter).count()) {
			if (Fetch()) {
				Announce(Snapshot());
			} else {
				wait = kRetryAfter;
			}
		}
		if (Chat::CancelableSleep(wait, canceled)) {
			break;
		}
	}
}

bool RateStore::Fetch()
{
	Http::HttpReq req;
	req.method = "GET";
	req.url = kEcbDailyUrl;
	req.timeoutSec = kFetchTimeoutSec;
	const Http::HttpResponse resp = Http::HttpRequest(req);
	std::string err;
	if (!resp.error.empty()) {
		HostLog(std::string("[fx] rates fetch failed: ") + resp.error);
		return false;
	}
	if (!Http::Require2xx(resp, "ECB rates", err)) {
		HostLog("[fx] " + err.substr(0, 200));
		return false;
	}
	RateTable table;
	if (!ParseEcbDaily(resp.body, table)) {
		HostLog("[fx] rates feed did not parse: " + Http::BodyForLog(resp.body, 120));
		return false;
	}
	const std::string line =
		"[fx] rates for " + table.date + " (" + std::to_string(table.perEur.size()) + " currencies)";
	{
		std::lock_guard<std::mutex> lock(mutex_);
		table_ = std::move(table);
		fetchedAtMs_ = TimeUtil::NowMs();
	}
	SaveCache();
	HostLog(line);
	return true;
}

void RateStore::LoadCache()
{
	const std::optional<std::string> text = FileUtil::ReadUtf8File(BraidcastConfigPath(kCacheFile));
	if (!text) {
		return;
	}
	const json j = json::parse(*text, nullptr, false);
	if (!j.is_object() || !j.contains("rates") || !j["rates"].is_object()) {
		HostLog("[fx] ignoring an unreadable rates cache");
		return;
	}
	RateTable table;
	table.date = j.value("date", std::string());
	for (const auto &[code, rate] : j["rates"].items()) {
		if (IsCurrencyCode(code) && rate.is_number() && rate.get<double>() > 0.0) {
			table.perEur[code] = rate.get<double>();
		}
	}
	if (DaysFromIsoDate(table.date) < 0 || table.perEur.size() < kMinRates) {
		HostLog("[fx] ignoring an incomplete rates cache");
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	table_ = std::move(table);
	fetchedAtMs_ = j.value("fetchedAt", static_cast<int64_t>(0));
}

void RateStore::SaveCache() const
{
	json j;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		j = json{{"date", table_.date}, {"fetchedAt", fetchedAtMs_}, {"rates", table_.perEur}};
	}
	const std::string body = j.dump(2);
	const FileUtil::AtomicWriteResult r =
		FileUtil::WriteBinaryFileAtomic(BraidcastConfigPath(kCacheFile), body.data(), body.size());
	if (r != FileUtil::AtomicWriteResult::Ok) {
		HostLog("[fx] could not save the rates cache");
	}
}

RateStore &Rates()
{
	static RateStore store;
	return store;
}

} // namespace Fx
