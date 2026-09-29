#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_ARCHIVE_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_ARCHIVE_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "../history/Db.hpp"
#include "../oauth/provider.hpp" // OAuth::DestinationId
#include "feed_query.hpp"

struct sqlite3;
struct sqlite3_stmt;

// chat.db: the multichat scrollback on disk, past the ring's 1000 messages and across
// restarts. ChatHistory (chat_history.hpp) admits every message and hands it here while
// persistence is on; a writer thread of the archive's own commits it in batches, and
// chat.list reads older pages back through a second connection once the ring runs out.
//
// What reaches disk is governed by the retention mode and by moderation. A platform
// whose deletes and bans the app does not yet honor (kModeratedPlatforms) keeps its rows
// for the current launch only, so nothing a moderator removed can outlive the launch that
// saw it. Every stored byte, the quarantined copies of a corrupt file included, is subject
// to retention, Clear, Off and the account purge.
//
// Threads and locks:
//   - Open runs on the main thread before any chat transport starts, and before the
//     writer thread exists.
//   - Enqueue, EnqueueRedact, EnqueueClear, SetRetention and Degrade are called with
//     ChatHistory's ring lock held, and take queueMutex_ after it: queue order is then
//     admission order, which is what makes PersistedSeq contiguous.
//   - PurgeAccount runs on TID_UI and only enqueues.
//   - The writer thread is started, under queueMutex_, only when there is something to
//     write or remove: never while retention has been Off since Open, when nothing is on
//     disk, unless Off left a file whose version it could not read, which the next control
//     op tries again. A failure to start it degrades the archive rather than throwing to
//     the caller, and Open then removes the store it opened and its quarantined copies,
//     since a Clear, a purge or Off may never reach them. What stays is owed: the next
//     control op that does start the writer removes it, whatever the mode.
//   - ReadOlder runs on the bridge's async lane. It copies the pending ops under
//     queueMutex_, releases it, then reads under readMutex_. The two are never held
//     together, and neither is ever held with the ring lock.
//   - The writer thread owns the write connection and holds no lock while it does I/O.
//     It takes readMutex_ only to close or reopen the read connection.
//   - queueMutex_ is a leaf.
//   - Shutdown runs on the main thread once the CEF message loop has returned and the chat
//     transports are stopped. It joins the writer, which first commits everything queued.
namespace Chat {

using json = nlohmann::json;

enum class Retention { Off, Session, SevenDays };

// Nothing is written until the retention setting exists to turn it off again.
inline constexpr Retention kDefaultRetention = Retention::Off;

enum class ArchiveStatus {
	Off,         // retention is Off: nothing stored, nothing read
	Ok,          // persisting
	Disabled,    // chat.db could not be used this launch (locked by another instance, unreadable)
	Recovered,   // a corrupt chat.db was set aside and a fresh one started
	NewerSchema, // chat.db belongs to a newer build and is left untouched
	Degraded,    // writes failed; the ring alone serves until the next launch, whatever the mode
};

enum class ModerationAction { Delete, ClearUser, ClearAll };

// One moderator action against a destination's chat: a message deleted, a user's
// messages removed (a ban or a timeout), or the whole chat cleared.
struct ModerationOp {
	OAuth::DestinationId dest;
	ModerationAction action = ModerationAction::Delete;
	std::string msgId;    // Delete
	std::string authorId; // ClearUser
};

// The `deleted` mark a redacted frame and its row carry: "message", "user" or "all".
const char *DeletedMark(ModerationAction action);

// A moderator action keyed the way held messages and stored rows are: by destination key
// (OAuth::DestinationKey). The one predicate the ring, the writer's queue and a reader's
// pending view all answer "does this remove that message" with.
//
// It reaches only messages admitted before it: seq below `belowSeq`, the ring's next seq
// when the op was applied. A user's first line after a timeout ends, or anything said
// after a clear, is a later message and stays.
struct Redaction {
	std::string dest;
	ModerationAction action = ModerationAction::Delete;
	std::string msgId, authorId;
	uint64_t belowSeq = 0;

	static Redaction From(const ModerationOp &op, uint64_t belowSeq);
	bool Matches(const std::string &destKey, uint64_t seq, const std::string &msgId,
		     const std::string &authorId) const;
	bool Matches(const std::string &destKey, uint64_t seq, const json &frame) const;
};

// The stable platform user id of a frame's author, "" when the platform gave none.
std::string FrameAuthorId(const json &frame);

// Strip a removed message's text and mark it deleted. The frame keeps its identity,
// author and time, so a dock can still show where it was.
void RedactFrame(json &frame, ModerationAction action);

// chat.db's schema ladder (chat_archive_schema.cpp).
extern const History::Ladder kChatLadder;

class ChatArchive {
public:
	// One admitted message as it is written: the columns, and the frame less seq/rx.
	// Built by MakeRow outside the ring lock; seq and rx are filled in under it.
	struct Row {
		uint64_t seq = 0;
		int64_t rx = 0;
		int64_t ts = 0;
		std::string dest, msgId, platform, accountId, profileUuid, authorId, body;
	};

	// The writer's op log, applied in queue order: a Clear removes only what was queued
	// before it, and a redaction reaches a row queued before it. Public only so Degrade
	// can hand the dropped queue back to be freed outside the ring lock.
	struct Clear {
		uint64_t belowSeq;
	};
	struct Purge {
		std::string accountId;
	};
	struct SetMode {
		Retention retention;
	};
	using Op = std::variant<Row, Redaction, Clear, Purge, SetMode>;
	using OpQueue = std::deque<Op>;

	// The newest retained rows, oldest-first, each with its destination key, and the first
	// seq the ring may issue.
	struct Seed {
		struct Entry {
			std::string dest;
			json frame;
		};
		std::vector<Entry> rows;
		uint64_t nextSeq = 1;
	};

	struct Options {
		std::string path; // chat.db
		Retention retention = kDefaultRetention;
		// Stamped on every row this process writes. A test simulates a relaunch by
		// opening the same file under a new one.
		std::string launchId;
		// The platforms whose rows may outlive their launch (kModeratedPlatforms).
		std::vector<std::string> moderatedPlatforms;
	};

	// Options for `path` with this process's launch id and moderated platforms.
	static Options DefaultOptions(const std::string &path, Retention retention = kDefaultRetention);

	ChatArchive() = default;
	~ChatArchive();
	ChatArchive(const ChatArchive &) = delete;
	ChatArchive &operator=(const ChatArchive &) = delete;

	// Open chat.db, set a corrupt one aside, apply retention, fill `seed`, and start the
	// writer. Synchronous; call once, before anything is admitted. With retention Off it
	// creates nothing, and removes what an earlier launch stored.
	void Open(const Options &options, Seed &seed);

	// `frame` (unstamped) as a row, or nothing when it cannot be serialized -- the message
	// is then kept in memory only, and logged, rather than failing a batch.
	static std::optional<Row> MakeRow(const OAuth::DestinationId &dest, const json &frame);

	// The ring-lock calls (see above). Each is a no-op once Shutdown has begun.
	void Enqueue(Row row);
	void EnqueueRedact(const Redaction &redaction);
	// Remove every row admitted before `belowSeq`, the ring's next seq at the Clear.
	void EnqueueClear(uint64_t belowSeq);
	// Change the mode. `lastIssuedSeq` is the newest seq the ring has issued: turning
	// persistence on marks everything up to it as settled, since none of it was queued.
	void SetRetention(Retention retention, uint64_t lastIssuedSeq);
	// Stop persisting for the rest of the launch and drop the queued rows, which are
	// returned so the caller can free them outside its lock.
	[[nodiscard]] OpQueue Degrade(const std::string &why);

	// Remove every stored row of an account (revocation). Any thread; only enqueues.
	void PurgeAccount(const std::string &accountId);

	// Up to `limit` stored messages matching `filter` with seq below `beforeSeq`,
	// newest-first, with every queued redaction, purge and Clear already applied. Nothing
	// while persistence is on but chat.db is not open for reading yet (it has just been
	// turned on): whether older messages exist is then not known.
	std::optional<std::vector<json>> ReadOlder(uint64_t beforeSeq, size_t limit, const Feed::Filter &filter);

	// Every queued row with seq at or below this is committed.
	uint64_t PersistedSeq() const { return persistedSeq_.load(std::memory_order_acquire); }
	// Rows are being queued and stored now.
	bool Active() const { return active_.load(std::memory_order_acquire); }
	ArchiveStatus Status() const;
	std::string StatusDetail() const;

	// Commit everything queued, stop the writer, close both connections and release the
	// lock file. Idempotent. Must run before static destruction on every orderly exit.
	void Shutdown();

	// The one log line for a launch that has gone to memory only.
	static void LogDegraded(const std::string &why);

	// Self-test hooks. While held, the writer takes nothing off the queue, so a test can
	// read with ops still pending; Shutdown overrides a hold.
	void HoldWrites(bool hold);
	// Wait until the queue is empty and no batch is in flight. False on timeout.
	bool WaitIdle(std::chrono::milliseconds timeout);
	// Whether the writer thread has been started.
	bool WriterStarted() const;
	// Make every later attempt to start the writer thread fail, as running out of threads
	// would.
	void FailWriterStart(bool fail);

	// The ring may grow past its cap while the writer catches up, up to this many rows.
	// Past it the archive degrades. Each held message costs about 3 KB as a parsed frame in
	// the ring and about 1 KB serialized in the queue (estimated from typical frames, not
	// measured), so this caps the pair at roughly 80 MB.
	static constexpr size_t kRingHardCap = 20000;

private:
	// What a reader must apply to rows it reads while the writer has not yet.
	using PendingOp = std::variant<Redaction, Purge>;

	void PushLocked(Op op);
	// Queue a control op (anything but a Row) to be written at once.
	void EnqueueControl(Op op);
	void PushControlLocked(Op op);
	// Stop queueing rows and take the queued ones out; they are returned to be freed
	// outside queueMutex_.
	[[nodiscard]] OpQueue DropRowsLocked();
	void StopPersisting();
	void DegradeFromWriter(const std::string &why);

	// Start the writer if it is not running. False when the thread could not be created:
	// the archive is then degraded and the queue dropped.
	bool StartWriterLocked();
	bool IsDegraded() const;
	// Off since Open, with nothing left on disk that this build may remove: no writer runs,
	// and a control op has nothing to reach.
	bool NothingToReachLocked() const;
	// RemoveStoreFiles for `why`; when a file stays, the removal is owed another try
	// (RemoveWhatIsOwed).
	void RemoveOrOwe(const std::string &why);
	// With the store closed: under Off, everything this build may remove; otherwise a
	// removal an earlier try owes. False when there was nothing to try.
	bool RemoveWhatIsOwed();
	std::string OwedRemoval() const;
	void SetOwedRemoval(std::string why);
	void WriterLoop();
	void Apply(std::vector<Op> &batch);
	// Write `ops` (no SetMode) as one transaction, retrying a failed commit.
	void Commit(std::vector<Op> &ops);
	bool TryCommit(const std::vector<Op> &ops, int64_t &countDelta, uint64_t &maxSeq, bool &rewrote);
	void ApplyMode(Retention retention);
	void Sweep();
	void Checkpoint();

	bool OpenStore();
	void CloseStore();
	bool AcquireLock();
	void ReleaseLock();
	// Remove chat.db and its -wal/-shm (RemoveOwnStore) and every quarantined copy, holding
	// the lock file for it; nothing while another instance holds that. `why` is logged. True
	// when a file this build may remove is still there, to be tried again.
	bool RemoveStoreFiles(const std::string &why);
	// Remove chat.db and its -wal/-shm, unless it is, or may be, a newer build's; `why` is
	// logged. True when it stays although it is not known to be a newer build's.
	bool RemoveOwnStore(const std::string &why);
	// Delete chat.db, -wal first, then -shm and the file. False, logged, if one stays.
	bool DeleteStoreFiles();
	// Session mode's answer to an unreadable chat.db: it holds only earlier launches' chat,
	// so it is deleted rather than set aside. The status detail, or "" when it could not be.
	std::string DiscardCorrupt();
	// Rename chat.db (and any -wal/-shm) aside; the new name, or "" when it failed.
	std::string Quarantine();
	// Delete the quarantined copies: all of them, or those past the retention window.
	void DeleteQuarantined(bool all);
	// The retention pass over the quarantined copies: in Session mode every copy goes,
	// since it holds an earlier launch's chat; otherwise those past the retention window.
	void SweepQuarantined();
	// Run `fn` holding the lock file, taken for the call when it is not held already. False,
	// with nothing run, while another instance holds it.
	bool WithStoreLock(const std::function<void()> &fn);
	// Each quarantined copy's path and the stamp its name carries.
	void
	ForEachQuarantined(const std::function<void(const std::filesystem::path &, const std::string &)> &visit) const;
	void BuildSeed(Seed &seed);
	void SetStatus(ArchiveStatus status, std::string detail);

	Options options_;
	Retention retention_ = Retention::Off; // the writer's copy of the mode (Open, then writer only)

	// Writer-owned (Open before the thread starts, the writer after, Shutdown after join).
	History::Db db_;
	sqlite3_stmt *insert_ = nullptr;
	void *lock_ = nullptr; // Win32 HANDLE of the lock file, null when not held
	int64_t count_ = 0;
	std::string writeError_; // why the last commit failed
	std::chrono::steady_clock::time_point nextSweep_{};

	// The read connection, opened after any quarantine and closed only under readMutex_.
	std::mutex readMutex_;
	sqlite3 *readDb_ = nullptr;

	mutable std::mutex queueMutex_;
	std::condition_variable wake_;
	std::condition_variable idle_;
	OpQueue ops_;
	std::deque<PendingOp> pending_; // queued redactions and purges, in queue order
	std::chrono::steady_clock::time_point firstQueuedAt_{};
	// The mode last asked for (Open, SetRetention).
	Retention mode_ = Retention::Off;
	// Why a removal that left a file this build may remove was made, or "" when none is
	// owed; the next control op tries again.
	std::string owedRemoval_;
	bool failWriterStart_ = false;
	bool urgent_ = false;  // a control op is queued: write it without the batching delay
	bool writing_ = false; // a batch is off the queue and not yet applied
	bool held_ = false;
	bool stopped_ = false;
	bool degraded_ = false;
	ArchiveStatus status_ = ArchiveStatus::Off;
	std::string statusDetail_;

	std::atomic<bool> active_{false};
	std::atomic<uint64_t> persistedSeq_{0};
	std::atomic<uint64_t> clearedBelow_{0};
	std::thread writer_;
};

// The process-wide archive (function-local static, like History()).
ChatArchive &Archive();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_ARCHIVE_HPP_
