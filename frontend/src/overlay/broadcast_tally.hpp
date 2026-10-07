#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_BROADCAST_TALLY_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_BROADCAST_TALLY_HPP_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>

#include <nlohmann/json.hpp>

#include "../events/event_model.hpp"
#include "../multistream/StorePaths.hpp"

// The running totals of one broadcast's events, for the widgets that count them
// (Overlay::CountsEvents). The overlay server's own record rather than a read of the event
// store: that store keeps 500 events across every platform, so a busy broadcast outgrows it
// and a page rebuilding its count from it on reload would come back short. Fed by exactly
// the events the server broadcasts live, so a page that stays connected and one that reloads
// count the same events.
//
// Raw sums only -- per (platform, type, kind): how many events, how many units (an event's
// count, or 1), and the sum of their amounts. Which of those a count reads is the page's rule
// (COUNTER_EVENT_SOURCES in web/src/overlay/counter.ts), so a new count needs no host change.
//
// The window is latched. It opens on the first frame that is live with a start time, at that
// time, and nothing moves it while the broadcast stays live -- the start the stream frame
// reports is recomputed from the outputs' uptimes and drifts. It closes at the first frame that
// is not live, at that moment. A closed window is final: an event arriving after the end is
// not counted, however early its own time.
//
// Persisted (the window and the sums only, never an event or an id) to overlay_tally.json, so
// the finished broadcast's figure is still there after a restart. A window left open by a
// crash or by quitting mid-broadcast is closed at the last save on load. A platform's figures
// go once its storage limit (Events::EventStore::MaxAgeMs) has passed since the window closed.
//
// Saved at most once per kSaveDebounceMs on the event path, so the last events of a burst are
// left unsaved by that path; SaveIfDue, on the app's 1 Hz tick, writes them. Every counted
// event is therefore on disk within kSaveDebounceMs plus one tick of being counted, and a
// failed write leaves the tally owed a save, retried at the same pace.
//
// Thread map: Add and OnStreamState on the overlay server's fan-out thread, each right before
// the frame it belongs to is sent (OverlayServer::Broadcast, BroadcastStreamState), or on the
// stopping thread after the fan-out is joined for a frame Stop left queued (JoinFanout);
// SaveIfDue on TID_UI (the bridge's stats tick); Snapshot and OpenSince on SSE connection
// threads and TID_UI; Open and Flush on TID_UI (server Start and Stop). All of it under
// mutex_, which is a leaf: nothing is called while it is held. Disk writes happen after it is
// released, ordered by writer_.
namespace Overlay {

class BroadcastTally {
public:
	// A tally that neither reads nor writes a file until Open.
	BroadcastTally() = default;
	BroadcastTally(const BroadcastTally &) = delete;
	BroadcastTally &operator=(const BroadcastTally &) = delete;

	// <config>/basic/overlay_tally.json.
	static std::string FilePath();

	// Load the record at `path` and save to it from now on. Once only; a later call is ignored.
	void Open(const std::string &path);

	// One event the server is broadcasting live. Counted when the window is open and the
	// event's time falls inside it; remembered either way, so a window opening just after
	// still counts an event that arrived just before its start was known.
	void Add(const Events::NormalizedEvent &ev);

	// One stream-state frame. Returns the tally as it now stands when the window opened or
	// closed -- taken in the same critical section, so it is exactly the moved window -- and
	// nullopt when it did not move.
	std::optional<nlohmann::json> OnStreamState(bool active, int64_t startedAtMs, int64_t nowMs);

	// The tally frame's body: {since, until, totals, recentIds}. `since` is null before any
	// broadcast was recorded; `until` is null while it is live. `recentIds` names the last
	// kRecentIds events counted, so a page that also received one of them live counts it once.
	nlohmann::json Snapshot() const;

	// The open window's start, or 0 when no broadcast is live.
	int64_t OpenSince() const;

	// Write a pending save now. For shutdown.
	void Flush();

	// Write a pending save once kSaveDebounceMs has passed since the last one: the save the
	// event path held back, or one that failed. Called on a timer.
	void SaveIfDue(int64_t nowMs);

	// How long recent broadcast events are remembered for a window that opens after them --
	// the gap between an output starting and the stream frame that reports its start, which
	// a busy channel can fill with more events than any small count -- and a cap on how many,
	// so a flood still costs bounded memory. Then how many counted ids a snapshot names.
	static constexpr int64_t kRecentWindowMs = 15 * 60 * 1000;
	static constexpr size_t kRecentEvents = 5000;
	static constexpr size_t kRecentIds = 64;
	// The least time between two saves on the event path and SaveIfDue. A transition and Flush
	// always save.
	static constexpr int64_t kSaveDebounceMs = 3000;

private:
	struct Sum {
		int64_t events = 0;
		int64_t units = 0;
		int64_t amount = 0;
	};
	using Key = std::tuple<std::string, std::string, std::string>; // platform, type, kind
	struct Recent {
		std::string id;
		Key key;
		int64_t ts = 0;
		int64_t units = 0;
		int64_t amount = 0;
		int64_t seenMs = 0; // when the app broadcast it, by its own clock
	};

	bool OpenLocked() const { return since_ > 0 && until_ == 0; }
	void CountLocked(const Recent &r);
	nlohmann::json SnapshotLocked() const;
	// The persisted record, stamped for writer_, and the file it goes to. Caller holds mutex_
	// and hands both to Persist once it is released.
	nlohmann::json RecordLocked(int64_t nowMs, std::string &path, uint64_t &stamp);
	// Whether a save is owed and kSaveDebounceMs has passed since the last attempt.
	bool SaveDueLocked(int64_t nowMs) const;
	// Write a record taken by RecordLocked. Caller does not hold mutex_.
	void Persist(const nlohmann::json &record, const std::string &path, uint64_t stamp);

	mutable std::mutex mutex_;
	std::string path_; // empty: in memory only
	int64_t since_ = 0;
	int64_t until_ = 0;
	std::map<Key, Sum> totals_;
	std::deque<Recent> recent_; // broadcast in the last kRecentWindowMs (at most kRecentEvents), counted or not
	std::deque<std::string> countedIds_; // the last kRecentIds counted
	bool dirty_ = false;
	int64_t lastSaveMs_ = 0;
	OrderedStoreSave writer_;
};

// How an event of `ev`'s type is told apart from its kind for counting: a YouTube membership
// is "milestone" when it names how many months it marks and "new" otherwise; every other event
// is "". Mirrored by eventKind in web/src/overlay/counter.ts.
std::string TallyKind(const Events::NormalizedEvent &ev);

} // namespace Overlay

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_BROADCAST_TALLY_HPP_
