#ifndef OBS_MULTISTREAM_FRONTEND_EVENTS_YOUTUBE_SUBSCRIBERS_SEEN_HPP_
#define OBS_MULTISTREAM_FRONTEND_EVENTS_YOUTUBE_SUBSCRIBERS_SEEN_HPP_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "event_store.hpp"
#include "../multistream/StorePaths.hpp"
#include "../util/time_util.hpp"

// Which YouTube subscribers have already been reported, per account.
//
// YouTube has no subscribe event. The transport reads the channel's newest subscribers
// (myRecentSubscribers) and stamps each with the time it read it, so the only thing that
// stops a subscriber being reported again on every read is the event store's dedupe -- and
// that store keeps 500 events across every platform and drops YouTube's after 30 days. A
// subscriber still among the newest few after their event has left the store came back as a
// brand-new follow, stamped now: a second alert, and a subscribe counted in a broadcast it
// did not happen in. This remembers the subscriber itself, independently of the store.
//
// Keyed by the subscriber's channel id, so unsubscribing and subscribing again is not a new
// follow either. Bounded twice: kMaxPerAccount most recently seen per account, and an entry
// not returned by any read for kMaxAge is dropped -- the YouTube API policy's storage limit,
// read from the event store's table of them. A subscriber still being returned is refreshed by
// every read, so it never ages out while it could still be re-reported.
//
// Nothing is kept for an account that is not connected. Removing one stops its transport
// without waiting for it, so a read already under way can finish after PurgeAccount; Observe
// asks `connected` under mutex_, and the removal drops the account before it purges, so that
// read either lands before the purge (and goes with it) or records nothing.
//
// Thread-safe: each YouTube account's transport runs on its own hub worker. A save is
// snapshotted under mutex_ and written without it; a save that fails stays owed and is retried
// by the next read of any account.
namespace Events {

class SeenSubscribers {
public:
	// Whether an account is still connected. Called under mutex_, so it must not call back in.
	using AccountCheck = std::function<bool(const std::string &accountId)>;

	// A store kept in the file at `path` (FilePath() for the app's own), or in memory alone
	// when `path` is empty. An empty `connected` counts every account as connected.
	explicit SeenSubscribers(std::string path = {}, AccountCheck connected = {});

	// <config>/basic/youtube_subscribers_seen.json.
	static std::string FilePath();

	// Record that one read of `accountId`'s newest subscribers returned `channelIds`, at
	// `nowMs`. Returns the ones never seen before, in the order given. The first read of an
	// account -- or the first after it went unread for kMaxAge -- also seeds it: see Seeded.
	// An account that is no longer connected records nothing and returns none.
	std::vector<std::string> Observe(const std::string &accountId, const std::vector<std::string> &channelIds,
					 int64_t nowMs);

	// Whether `accountId` has been read within kMaxAge of `nowMs`. Until it has, everything a
	// read returns is the channel's existing audience rather than news, so a caller reports
	// none of it as a live follow.
	bool Seeded(const std::string &accountId, int64_t nowMs) const;

	// Drop everything kept for `accountId` and save, for the account's removal. True when
	// there was anything.
	bool PurgeAccount(const std::string &accountId);

	static constexpr size_t kMaxPerAccount = 2000;
	static constexpr int64_t kMaxAge = EventStore::MaxAgeMs("youtube");
	// How stale a kept time may get before a read that refreshes it is worth a save. Far
	// inside kMaxAge, so a subscriber still being returned never ages out between saves.
	static constexpr int64_t kRefreshSaveAfter = TimeUtil::kDayMs;

private:
	struct Account {
		int64_t readMs = 0;                              // the last read of this account
		std::unordered_map<std::string, int64_t> seenMs; // channel id -> last returned
	};

	void Load();
	// Drop what has aged past kMaxAge and, past kMaxPerAccount, the least recently seen.
	// True when anything went. Caller holds mutex_.
	static bool TrimLocked(Account &a, int64_t nowMs);
	// Stamp and snapshot the whole store. Caller holds mutex_; Save it with mutex_ released.
	nlohmann::json SnapshotLocked(uint64_t &stamp);
	void Save(const nlohmann::json &root, uint64_t stamp);

	const std::string path_; // empty: in memory only
	const AccountCheck connected_;
	mutable std::mutex mutex_;
	std::map<std::string, Account> accounts_;
	bool unsaved_ = false; // a save failed; the next Observe saves even with nothing new
	OrderedStoreSave writer_{SaveHistory::Drop};
};

// The app's store, created on first use.
SeenSubscribers &YouTubeSubscribersSeen();

} // namespace Events

#endif // OBS_MULTISTREAM_FRONTEND_EVENTS_YOUTUBE_SUBSCRIBERS_SEEN_HPP_
