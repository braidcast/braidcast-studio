#include "youtube_subscribers_seen.hpp"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

namespace Events {

using json = nlohmann::json;

SeenSubscribers::SeenSubscribers(std::string path) : path_(std::move(path))
{
	Load();
}

std::string SeenSubscribers::FilePath()
{
	return MultistreamBasicPath("youtube_subscribers_seen.json");
}

void SeenSubscribers::Load()
{
	// Called from the ctor before `this` is visible to any other thread, so no lock.
	DropStoreHistory(path_);
	const json root = LoadStoreJson(path_, OnUnusable::Leave);
	const json *accounts = root.contains("accounts") ? &root["accounts"] : nullptr;
	if (!accounts || !accounts->is_object()) {
		return;
	}
	const int64_t now = TimeUtil::NowMs();
	for (const auto &[accountId, entry] : accounts->items()) {
		if (!entry.is_object()) {
			continue;
		}
		Account a;
		a.readMs = entry.value("read", static_cast<int64_t>(0));
		const auto seen = entry.find("seen");
		if (seen != entry.end() && seen->is_object()) {
			for (const auto &[channelId, ms] : seen->items()) {
				if (ms.is_number_integer()) {
					a.seenMs[channelId] = ms.get<int64_t>();
				}
			}
		}
		TrimLocked(a, now);
		if (a.readMs > 0 || !a.seenMs.empty()) {
			accounts_[accountId] = std::move(a);
		}
	}
}

bool SeenSubscribers::TrimLocked(Account &a, int64_t nowMs)
{
	const size_t before = a.seenMs.size();
	for (auto it = a.seenMs.begin(); it != a.seenMs.end();) {
		it = nowMs - it->second > kMaxAge ? a.seenMs.erase(it) : std::next(it);
	}
	if (a.seenMs.size() > kMaxPerAccount) {
		std::vector<std::pair<int64_t, std::string>> byAge;
		byAge.reserve(a.seenMs.size());
		for (const auto &[channelId, ms] : a.seenMs) {
			byAge.emplace_back(ms, channelId);
		}
		const size_t excess = a.seenMs.size() - kMaxPerAccount;
		std::nth_element(byAge.begin(), byAge.begin() + static_cast<std::ptrdiff_t>(excess), byAge.end());
		for (size_t i = 0; i < excess; ++i) {
			a.seenMs.erase(byAge[i].second);
		}
	}
	return a.seenMs.size() != before;
}

std::vector<std::string> SeenSubscribers::Observe(const std::string &accountId,
						  const std::vector<std::string> &channelIds, int64_t nowMs)
{
	std::vector<std::string> fresh;
	json snapshot;
	uint64_t stamp = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto [it, created] = accounts_.try_emplace(accountId);
		Account &a = it->second;
		// Aged entries go first, so a subscriber past kMaxAge reads as new rather than
		// being refreshed back into the store.
		bool changed = TrimLocked(a, nowMs) || created || nowMs - a.readMs > kRefreshSaveAfter;
		a.readMs = nowMs;
		for (const std::string &channelId : channelIds) {
			if (channelId.empty()) {
				continue;
			}
			auto [seen, isNew] = a.seenMs.try_emplace(channelId, nowMs);
			if (isNew) {
				fresh.push_back(channelId);
				changed = true;
			} else {
				changed = changed || nowMs - seen->second > kRefreshSaveAfter;
				seen->second = nowMs;
			}
		}
		changed = TrimLocked(a, nowMs) || changed;
		if (!changed || path_.empty()) {
			return fresh;
		}
		snapshot = SnapshotLocked(stamp);
	}
	Save(snapshot, stamp);
	return fresh;
}

bool SeenSubscribers::Seeded(const std::string &accountId, int64_t nowMs) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = accounts_.find(accountId);
	return it != accounts_.end() && it->second.readMs > 0 && nowMs - it->second.readMs <= kMaxAge;
}

bool SeenSubscribers::PurgeAccount(const std::string &accountId)
{
	json snapshot;
	uint64_t stamp = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (accounts_.erase(accountId) == 0) {
			return false;
		}
		if (path_.empty()) {
			return true;
		}
		snapshot = SnapshotLocked(stamp);
	}
	Save(snapshot, stamp);
	return true;
}

json SeenSubscribers::SnapshotLocked(uint64_t &stamp)
{
	json accounts = json::object();
	for (const auto &[accountId, a] : accounts_) {
		json seen = json::object();
		for (const auto &[channelId, ms] : a.seenMs) {
			seen[channelId] = ms;
		}
		accounts[accountId] = {{"read", a.readMs}, {"seen", std::move(seen)}};
	}
	stamp = writer_.Stamp();
	return {{"version", 1}, {"accounts", std::move(accounts)}};
}

void SeenSubscribers::Save(const json &root, uint64_t stamp)
{
	writer_.Write(root, path_, stamp);
}

SeenSubscribers &YouTubeSubscribersSeen()
{
	static SeenSubscribers store(SeenSubscribers::FilePath());
	return store;
}

} // namespace Events
