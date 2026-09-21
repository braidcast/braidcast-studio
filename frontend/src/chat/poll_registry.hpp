#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_POLL_REGISTRY_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_POLL_REGISTRY_HPP_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../oauth/provider.hpp" // OAuth::DestinationId

// The live polls this session has opened, by poll id, each with the destination whose
// broadcast holds it. In memory only: a poll lives inside one broadcast, so nothing about it
// is worth restoring after a restart.
//
// Mutex-guarded because polls.create / polls.end run on the async bridge lane's worker
// threads while polls.list / polls.dismiss run on the UI thread. Every mutation pushes
// `polls.changed` with the full {polls:[...]} list, delivered in mutation order (see
// EmitChanged); the state lock is never held across that emit or across any network call
// (the bridge does the I/O between two registry calls).
//
// Wire shape of one poll:
//   {id, accountId, profileUuid, question, options:[{text, tally:number|null}],
//    status:"active"|"closed", startedAtMs, endedAtMs:number|null, error?:string}
namespace Chat {

// The option counts a poll may carry. YouTube's limits, and the only platform with polls
// today; a platform with a different range would make these per-transport.
inline constexpr size_t kMinPollOptions = 2;
inline constexpr size_t kMaxPollOptions = 4;

class PollRegistry {
public:
	enum class DismissResult { Removed, Unknown, StillRunning };

	PollRegistry() = default;
	PollRegistry(const PollRegistry &) = delete;
	PollRegistry &operator=(const PollRegistry &) = delete;

	// Register a poll `dest`'s transport just opened. `reported` is the transport's poll
	// shape (see ChatTransport::createPoll); a question or option list it left empty falls
	// back to what was asked for. Returns the registered poll.
	nlohmann::json Open(const OAuth::DestinationId &dest, const std::string &question,
			    const std::vector<std::string> &options, const nlohmann::json &reported);

	// One poll as the wire shape, or null when unknown.
	nlohmann::json Get(const std::string &id) const;

	// Mark `id` closed now with whatever tallies `reported` carries, clearing any earlier
	// error. Returns the poll, or null when it was dismissed meanwhile.
	nlohmann::json Close(const std::string &id, const nlohmann::json &reported);

	// Record why the last action on `id` failed, leaving it in place for the streamer to
	// retry or dismiss. Returns the poll, or null when unknown.
	nlohmann::json Fail(const std::string &id, const std::string &error);

	// Remove `id` when it is closed, carries an error, or `orphaned` (its broadcast has no
	// live chat any more). A poll that is still running and reachable stays: dropping it
	// would leave it pinned in the chat with no way to end it from here.
	DismissResult Dismiss(const std::string &id, bool orphaned);

	// {polls:[...]}, most recently started first.
	nlohmann::json List() const;

private:
	struct Poll {
		std::string id;
		OAuth::DestinationId dest;
		std::string question;
		nlohmann::json options = nlohmann::json::array(); // [{text, tally}]
		std::string status;
		int64_t startedAtMs = 0;
		std::optional<int64_t> endedAtMs;
		std::string error;
	};

	static nlohmann::json ToJson(const Poll &poll);
	nlohmann::json ListLocked() const;
	std::vector<Poll>::iterator FindLocked(const std::string &id);
	const Poll *FindLocked(const std::string &id) const;

	// Emit the list as it stands after a mutation. Every mutator takes `emitMutex_` BEFORE
	// the state lock and holds it through this call, and this call always appends to the UI
	// task queue (AsyncTask::QueueOnUi), never delivers inline -- so snapshots reach the UI
	// in mutation order whichever thread mutated. The state lock is released before it.
	void EmitChanged(const nlohmann::json &list);

	std::mutex emitMutex_;
	mutable std::mutex mutex_;
	std::vector<Poll> polls_; // in the order they were opened
};

PollRegistry &Polls();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_POLL_REGISTRY_HPP_
