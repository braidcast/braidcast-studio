#include "chat_archive.hpp"

#include <windows.h>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iterator>
#include <system_error>
#include <utility>

#include "../log.hpp"
#include "../util/json_util.hpp"
#include "../util/text_encoding.hpp"
#include "../util/time_util.hpp"
#include "chat_history.hpp"

namespace Chat {

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// The platforms whose deletes and bans the app applies to stored chat (§3 of the design).
// Until a platform is listed here its rows are removed at the next launch, so a message a
// moderator removed while the app was not listening can outlive only the launch that saw
// it. Each moderation commit adds its platform.
constexpr std::array<std::string_view, 2> kModeratedPlatforms{"twitch", "youtube"};

// The longest any row, or a quarantined copy of chat.db, is kept, in every mode. YouTube's
// policy allows 30 days; this stays well under it.
constexpr int64_t kRetentionMs = 7 * TimeUtil::kDayMs;
constexpr int64_t kMaxRows = 10000;
// The running count past which the writer sweeps without waiting for the hourly one.
constexpr int64_t kSweepAtRows = 10500;
constexpr auto kSweepInterval = 1h;

// A batch is written once it holds this many rows, or this long after its first row was
// queued, whichever comes first. A control op (Clear, a redaction, a purge, a mode change)
// is written at once.
constexpr size_t kBatchRows = 256;
constexpr auto kBatchDelay = 250ms;

// A failed commit is retried after each of these pauses, then the archive degrades.
constexpr std::chrono::milliseconds kRetryBackoff[] = {200ms, 800ms, 2000ms};

// A TRUNCATE checkpoint waits for readers; a busy one is retried this many times and then
// left for the next, never counted as a write failure.
constexpr int kCheckpointAttempts = 5;
constexpr auto kCheckpointPause = 50ms;

constexpr const char *kQuarantineInfix = ".corrupt-";
constexpr const char *kLockSuffix = ".lock";
constexpr const char *kSideFiles[] = {"-wal", "-shm"};

// Why stored chat is removed, as logged.
constexpr const char *kOffReason = "chat history is off";
constexpr const char *kAbandonedReason = "it cannot be kept current this launch";
constexpr const char *kSessionReason = "chat history is kept for this session only";

constexpr int kBusyTimeoutMs = 3000;

const char *StatusName(ArchiveStatus status)
{
	switch (status) {
	case ArchiveStatus::Off:
		return "off";
	case ArchiveStatus::Ok:
		return "ok";
	case ArchiveStatus::Disabled:
		return "disabled";
	case ArchiveStatus::Recovered:
		return "recovered";
	case ArchiveStatus::NewerSchema:
		return "newer-schema";
	case ArchiveStatus::Degraded:
		return "degraded";
	case ArchiveStatus::UnknownSetting:
		return "unknown-setting";
	case ArchiveStatus::Unreadable:
		return "unreadable";
	}
	return "unknown";
}

// A prepared statement that finalizes itself.
class Statement {
public:
	Statement(sqlite3 *db, const std::string &sql)
	{
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
			sqlite3_finalize(stmt_);
			stmt_ = nullptr;
		}
	}
	~Statement() { sqlite3_finalize(stmt_); }
	Statement(const Statement &) = delete;
	Statement &operator=(const Statement &) = delete;

	explicit operator bool() const { return stmt_ != nullptr; }
	sqlite3_stmt *get() const { return stmt_; }

	void Bind(int index, const std::string &value)
	{
		sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
	}
	void Bind(int index, int64_t value) { sqlite3_bind_int64(stmt_, index, value); }
	int Step() { return sqlite3_step(stmt_); }

private:
	sqlite3_stmt *stmt_ = nullptr;
};

std::string ColumnText(sqlite3_stmt *stmt, int column)
{
	const unsigned char *text = sqlite3_column_text(stmt, column);
	return text ? std::string(reinterpret_cast<const char *>(text),
				  static_cast<size_t>(sqlite3_column_bytes(stmt, column)))
		    : std::string();
}

// A seq as SQLite stores it. Seqs never approach INT64_MAX; a "below everything" bound
// that does is clamped rather than wrapped negative.
int64_t SqlSeq(uint64_t seq)
{
	return seq > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(seq);
}

// Failures that say the file or the disk is unusable, as opposed to one row being unfit.
// A batch that meets one is rolled back and retried; any other failure skips the row.
bool IsStoreFailure(int rc)
{
	switch (rc & 0xff) {
	case SQLITE_IOERR:
	case SQLITE_FULL:
	case SQLITE_CORRUPT:
	case SQLITE_NOTADB:
	case SQLITE_NOMEM:
	case SQLITE_BUSY:
	case SQLITE_LOCKED:
	case SQLITE_READONLY:
	case SQLITE_CANTOPEN:
	case SQLITE_PROTOCOL:
		return true;
	default:
		return false;
	}
}

bool IsCorruptFile(int rc)
{
	return (rc & 0xff) == SQLITE_NOTADB || (rc & 0xff) == SQLITE_CORRUPT;
}

// A stored row back as the frame the ring held: the body with its seq, receipt time and
// deleted mark restored from the columns. Nothing for a body that no longer parses.
std::optional<json> FrameFromRow(sqlite3_stmt *stmt, int seqCol, int rxCol, int deletedCol, int bodyCol)
{
	json frame = JsonUtil::ParseJson(ColumnText(stmt, bodyCol));
	if (!frame.is_object()) {
		DBG(LogCat::Chat, "chat-archive: stored row %lld does not parse; skipped",
		    static_cast<long long>(sqlite3_column_int64(stmt, seqCol)));
		return std::nullopt;
	}
	frame["seq"] = static_cast<uint64_t>(sqlite3_column_int64(stmt, seqCol));
	frame["rx"] = sqlite3_column_int64(stmt, rxCol);
	const std::string deleted = ColumnText(stmt, deletedCol);
	if (!deleted.empty()) {
		frame["deleted"] = deleted;
	}
	return frame;
}

// Raise `value` to at least `to`.
void RaiseTo(std::atomic<uint64_t> &value, uint64_t to)
{
	uint64_t seen = value.load(std::memory_order_relaxed);
	while (seen < to && !value.compare_exchange_weak(seen, to, std::memory_order_acq_rel)) {
	}
}

// The local time a quarantined copy was set aside, from the LocalFileStamp in its name.
std::optional<std::time_t> QuarantineTime(const std::string &stamp)
{
	std::tm tm{};
	if (std::sscanf(stamp.c_str(), "%d-%d-%d_%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
			&tm.tm_min, &tm.tm_sec) != 6) {
		return std::nullopt;
	}
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	tm.tm_isdst = -1;
	const std::time_t t = std::mktime(&tm);
	return t == static_cast<std::time_t>(-1) ? std::nullopt : std::optional<std::time_t>(t);
}

} // namespace

const char *DeletedMark(ModerationAction action)
{
	switch (action) {
	case ModerationAction::Delete:
		return "message";
	case ModerationAction::ClearUser:
		return "user";
	case ModerationAction::ClearAll:
		return "all";
	}
	return "message";
}

int64_t FrameTs(const json &frame)
{
	const auto ts = frame.find("ts");
	return ts != frame.end() && ts->is_number() ? ts->get<int64_t>() : 0;
}

std::string FrameAuthorId(const json &frame)
{
	const auto author = frame.find("author");
	return author != frame.end() ? JsonUtil::Str(*author, "id") : std::string();
}

void RedactFrame(json &frame, ModerationAction action)
{
	frame["fragments"] = json::array();
	frame["deleted"] = DeletedMark(action);
}

bool SafeToReplay(const ModerationOp &op)
{
	return op.action == ModerationAction::Delete || op.beforeTs.has_value();
}

Redaction Redaction::From(const ModerationOp &op, uint64_t belowSeq)
{
	return Redaction{OAuth::DestinationKey(op.dest), op.action, op.msgId, op.authorId, belowSeq, op.beforeTs};
}

bool Redaction::Matches(const std::string &destKey, uint64_t seq, int64_t ts, const std::string &id,
			const std::string &author) const
{
	if (destKey != dest || seq >= belowSeq) {
		return false;
	}
	if (beforeTs && !(ts > 0 && ts <= *beforeTs)) {
		return false;
	}
	switch (action) {
	case ModerationAction::Delete:
		return !msgId.empty() && id == msgId;
	case ModerationAction::ClearUser:
		return !authorId.empty() && author == authorId;
	case ModerationAction::ClearAll:
		return true;
	}
	return false;
}

bool Redaction::Matches(const std::string &destKey, uint64_t seq, const json &frame) const
{
	return Matches(destKey, seq, FrameTs(frame), JsonUtil::Str(frame, "id"), FrameAuthorId(frame));
}

ChatArchive::Options ChatArchive::DefaultOptions(const std::string &path, Retention retention)
{
	Options options;
	options.path = path;
	options.retention = retention;
	options.launchId = LaunchId();
	options.moderatedPlatforms.assign(kModeratedPlatforms.begin(), kModeratedPlatforms.end());
	return options;
}

ChatArchive::~ChatArchive()
{
	// Joining here could wait on a writer stuck in I/O while the process is exiting, and
	// destroying a joinable thread terminates. Shutdown is what stops the writer; reaching
	// this with it still running is a missed Shutdown, and the writer is abandoned.
	if (writer_.joinable()) {
		writer_.detach();
		HostLog("[chat-archive] Shutdown was skipped; the writer thread was abandoned");
		return;
	}
	CloseStore();
	ReleaseLock();
}

void ChatArchive::Open(const Options &options, Seed &seed, SeqRaise raiseSeq)
{
	const auto started = std::chrono::steady_clock::now();
	options_ = options;
	raiseSeq_ = std::move(raiseSeq);
	const bool unknownSetting = options.unknownSetting.has_value();
	// An unknown setting runs as 7 days, the longest a known one keeps chat, bar the rows.
	retention_ = unknownSetting ? Retention::SevenDays : options.retention;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		mode_ = retention_;
		unknownSetting_ = unknownSetting;
	}
	bool opened = false;
	if (retention_ == Retention::Off) {
		SetStatus(ArchiveStatus::Off, {});
		// Off was not chosen this launch, so a store another instance holds is its to manage.
		RemoveOrOwe(kOffReason, false);
	} else if (OpenStore()) {
		opened = true;
		Sweep();
		BuildSeed(seed);
		persistedSeq_.store(seed.nextSeq - 1, std::memory_order_release);
		if (unknownSetting) {
			SetStatus(ArchiveStatus::UnknownSetting, *options.unknownSetting);
		} else {
			active_.store(true, std::memory_order_release);
		}
	} else if (Status() == ArchiveStatus::NewerSchema) {
		// chat.db stays exactly as the newer build left it; the copies this one set aside
		// still age out.
		WithStoreLock([this] { SweepQuarantined(); });
	}
	const int64_t kept = count_; // the writer owns count_ once it runs
	bool writing = true;
	if (opened) {
		std::lock_guard<std::mutex> lock(queueMutex_);
		writing = StartWriterLocked();
	}
	if (!writing) {
		LogDegraded(StatusDetail());
		// A Clear, a purge or Off may never reach the store opened above, so it goes now,
		// with its quarantined copies, rather than outlive them; the lock is still held.
		// The ring keeps what was seeded from it.
		CloseStore();
		RemoveOrOwe(kAbandonedReason, true);
		ReleaseLock();
	}
	const auto ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
			.count();
	HostLog("[chat-archive] open: " + std::string(StatusName(Status())) + ", " + std::to_string(kept) +
		" row(s) kept, " + std::to_string(seed.rows.size()) + " seeded, " + std::to_string(ms) + " ms" +
		(StatusDetail().empty() ? std::string() : " (" + StatusDetail() + ")"));
}

std::optional<ChatArchive::Row> ChatArchive::MakeRow(const OAuth::DestinationId &dest, const json &frame)
{
	Row row;
	// ChatHistory::Add has already repaired invalid UTF-8, so this does not throw on the
	// normal path; if it does, the message is skipped here rather than failing a batch.
	try {
		row.body = frame.dump();
	} catch (const std::exception &e) {
		HostLog(std::string("[chat-archive] a ") + JsonUtil::Str(frame, "platform") +
			" message could not be serialized and is kept in memory only: " + e.what());
		return std::nullopt;
	}
	row.dest = OAuth::DestinationKey(dest);
	row.msgId = JsonUtil::Str(frame, "id");
	row.platform = Feed::NormalizePlatform(JsonUtil::Str(frame, "platform"));
	row.accountId = JsonUtil::Str(frame, "accountId");
	row.profileUuid = JsonUtil::Str(frame, "profileUuid");
	row.authorId = FrameAuthorId(frame);
	row.ts = FrameTs(frame);
	return row;
}

void ChatArchive::PushLocked(Op op)
{
	if (ops_.empty()) {
		firstQueuedAt_ = std::chrono::steady_clock::now();
	}
	ops_.push_back(std::move(op));
}

void ChatArchive::Enqueue(Row row)
{
	std::lock_guard<std::mutex> lock(queueMutex_);
	if (stopped_ || degraded_ || !active_.load(std::memory_order_acquire)) {
		return;
	}
	PushLocked(std::move(row));
	if (ops_.size() == 1 || ops_.size() >= kBatchRows) {
		wake_.notify_one();
	}
}

void ChatArchive::EnqueueRedact(const Redaction &redaction)
{
	EnqueueControl(redaction);
}

void ChatArchive::EnqueueClear(uint64_t belowSeq)
{
	EnqueueControl(Clear{belowSeq});
}

void ChatArchive::PurgeAccount(const std::string &accountId)
{
	if (!accountId.empty()) {
		EnqueueControl(Purge{accountId});
	}
}

void ChatArchive::EnqueueControl(Op op)
{
	bool started = true;
	bool overflowed = false;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		if (stopped_) {
			return;
		}
		if (NothingToReachLocked()) {
			// No writer runs to take it, but a store another instance held at Open may still
			// be adopted later; it is kept for that (with no writer, missed_ is this lock's).
			overflowed = KeepMissed(op);
		} else {
			PushControlLocked(std::move(op));
			started = StartWriterLocked();
		}
	}
	if (overflowed) {
		LogMissedOverflow();
	}
	if (!started) {
		LogDegraded(StatusDetail());
	}
}

void ChatArchive::PushControlLocked(Op op)
{
	// A reader applies a queued redaction, purge or Clear to what it reads until the writer
	// has.
	if (const Redaction *redaction = std::get_if<Redaction>(&op)) {
		pending_.push_back(*redaction);
	} else if (const Purge *purge = std::get_if<Purge>(&op)) {
		pending_.push_back(*purge);
	} else if (const Clear *clear = std::get_if<Clear>(&op)) {
		pending_.push_back(*clear);
	}
	PushLocked(std::move(op));
	++controlsQueued_;
	urgent_ = true;
	wake_.notify_one();
}

void ChatArchive::SetRetention(Retention retention, uint64_t lastIssuedSeq)
{
	bool started = true;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		if (stopped_) {
			return;
		}
		const bool offSinceOpen = NothingToReachLocked();
		if (unknownSetting_) {
			unknownSetting_ = false;
			if (status_ == ArchiveStatus::UnknownSetting) {
				status_ = ArchiveStatus::Ok;
				statusDetail_.clear();
			}
		}
		mode_ = retention;
		if (retention == Retention::Off) {
			active_.store(false, std::memory_order_release);
			if (offSinceOpen) {
				return;
			}
		} else if (!active_.load(std::memory_order_acquire) && !degraded_) {
			// Nothing up to lastIssuedSeq was queued, so none of it is owed to the disk;
			// without this the ring front would wait for a commit that can never come.
			RaiseTo(persistedSeq_, lastIssuedSeq);
			active_.store(true, std::memory_order_release);
		}
		PushControlLocked(SetMode{retention});
		started = StartWriterLocked();
	}
	if (!started) {
		LogDegraded(StatusDetail());
	}
}

bool ChatArchive::StartWriterLocked()
{
	// stopped_ first: once it is set, Shutdown may be joining writer_ outside the lock.
	if (stopped_ || writer_.joinable()) {
		return true;
	}
	try {
		if (failWriterStart_) {
			throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
						"self-test");
		}
		writer_ = std::thread([this] { WriterLoop(); });
		return true;
	} catch (const std::system_error &e) {
		// Nothing would ever write or remove what is queued, so it goes, and the ring alone
		// serves for the rest of the launch.
		active_.store(false, std::memory_order_release);
		ops_.clear();
		pending_.clear();
		controlsDone_ = controlsQueued_;
		urgent_ = false;
		if (!degraded_) {
			degraded_ = true;
			status_ = ArchiveStatus::Degraded;
			statusDetail_ = std::string("the writer thread could not be started: ") + e.what();
		}
		idle_.notify_all();
		return false;
	}
}

bool ChatArchive::NothingToReachLocked() const
{
	// Open removed everything this build may touch, bar a file it could not, which is owed
	// another try.
	return mode_ == Retention::Off && !writer_.joinable() && owedRemoval_.empty();
}

void ChatArchive::RemoveOrOwe(const std::string &why, bool oweWhileLocked)
{
	// RemoveStoreFiles leaves `unreadable` as it is when another instance holds the store or the
	// check for it failed, since nothing was probed. Only the thread that removes (Open's, then the writer) writes the flag,
	// so the copy read here is still current when it is written back.
	bool unreadable = false;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		unreadable = unreadableKept_;
	}
	const bool left = RemoveStoreFiles(why, oweWhileLocked, unreadable);
	std::lock_guard<std::mutex> lock(queueMutex_);
	owedRemoval_ = left ? why : std::string();
	unreadableKept_ = left && unreadable;
}

bool ChatArchive::RemoveWhatIsOwed()
{
	if (db_.IsOpen()) {
		return false;
	}
	const std::string why = retention_ == Retention::Off ? std::string(kOffReason) : OwedRemoval();
	if (why.empty()) {
		return false;
	}
	RemoveOrOwe(why, true);
	return true;
}

std::string ChatArchive::OwedRemoval() const
{
	std::lock_guard<std::mutex> lock(queueMutex_);
	return owedRemoval_;
}

void ChatArchive::SetOwedRemoval(std::string why)
{
	std::lock_guard<std::mutex> lock(queueMutex_);
	owedRemoval_ = std::move(why);
	if (owedRemoval_.empty()) {
		unreadableKept_ = false;
	}
}

void ChatArchive::FailWrites(bool fail)
{
	failWrites_.store(fail, std::memory_order_release);
}

void ChatArchive::FailWriterStart(bool fail)
{
	std::lock_guard<std::mutex> lock(queueMutex_);
	failWriterStart_ = fail;
}

bool ChatArchive::IsDegraded() const
{
	std::lock_guard<std::mutex> lock(queueMutex_);
	return degraded_;
}

void ChatArchive::LogDegraded(const std::string &why)
{
	HostLog("[chat-archive] degraded, chat history is in memory only until the next launch: " + why);
}

ChatArchive::OpQueue ChatArchive::DropRowsLocked()
{
	active_.store(false, std::memory_order_release);
	OpQueue dropped;
	OpQueue kept;
	for (Op &op : ops_) {
		(std::holds_alternative<Row>(op) ? dropped : kept).push_back(std::move(op));
	}
	ops_.swap(kept);
	return dropped;
}

ChatArchive::OpQueue ChatArchive::Degrade(const std::string &why)
{
	std::lock_guard<std::mutex> lock(queueMutex_);
	if (degraded_ || stopped_) {
		return {};
	}
	degraded_ = true;
	status_ = ArchiveStatus::Degraded;
	statusDetail_ = why;
	return DropRowsLocked();
}

void ChatArchive::DegradeFromWriter(const std::string &why)
{
	OpQueue dropped; // freed after the lock is released
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		if (degraded_) {
			return;
		}
		degraded_ = true;
		status_ = ArchiveStatus::Degraded;
		statusDetail_ = why;
		dropped = DropRowsLocked();
	}
	LogDegraded(why);
}

void ChatArchive::StopPersisting()
{
	OpQueue dropped; // freed after the lock is released
	std::lock_guard<std::mutex> lock(queueMutex_);
	dropped = DropRowsLocked();
}

std::optional<std::vector<json>> ChatArchive::ReadOlder(uint64_t beforeSeq, size_t limit, const Feed::Filter &filter)
{
	std::vector<json> out;
	if (limit == 0) {
		return out;
	}
	// Copied before the read, so an op the writer commits in between is either still in
	// this copy or already in what the read sees -- never in neither.
	std::deque<PendingOp> pending;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		pending = pending_;
	}

	std::lock_guard<std::mutex> lock(readMutex_);
	if (!readDb_) {
		// Active with no read connection: the writer has not opened chat.db yet. Every
		// other way to lose it (Off, Degraded, a failed open, Shutdown) clears Active first.
		if (Active()) {
			return std::nullopt;
		}
		return out;
	}
	// Bounds read once the store is published, so a store adopted since the copy lifts them.
	const uint64_t clearedBelow = ClearedBelow(pending);
	for (PendingOp &op : pending) {
		if (Redaction *redaction = std::get_if<Redaction>(&op)) {
			redaction->belowSeq = ReachBelow(redaction->belowSeq);
		}
	}
	std::string sql = "SELECT seq, rx, deleted, body, dest, msg_id, author_id, account_id, ts FROM messages "
			  "WHERE seq < ?1 AND seq >= ?2";
	switch (filter.kind) {
	case Feed::Filter::Kind::All:
		break;
	case Feed::Filter::Kind::Platform:
		sql += " AND platform = ?3";
		break;
	case Feed::Filter::Kind::Destination:
		sql += " AND (profile_uuid = ?3 OR (profile_uuid = '' AND account_id = ?4))";
		break;
	}
	// No LIMIT: rows a pending purge drops must not shorten the page, so the scan steps
	// until it has `limit` survivors. It streams newest-first off the key or an index.
	sql += " ORDER BY seq DESC";
	Statement stmt(readDb_, sql);
	if (!stmt) {
		DBG(LogCat::Chat, "chat-archive: read prepare failed: %s", sqlite3_errmsg(readDb_));
		return out;
	}
	stmt.Bind(1, SqlSeq(beforeSeq));
	stmt.Bind(2, SqlSeq(clearedBelow));
	if (filter.kind == Feed::Filter::Kind::Platform) {
		stmt.Bind(3, filter.platform);
	} else if (filter.kind == Feed::Filter::Kind::Destination) {
		stmt.Bind(3, filter.profileUuid);
		stmt.Bind(4, filter.accountId);
	}
	int rc = SQLITE_ROW;
	while (out.size() < limit && (rc = stmt.Step()) == SQLITE_ROW) {
		const std::string accountId = ColumnText(stmt.get(), 7);
		const bool purged = std::any_of(pending.begin(), pending.end(), [&](const PendingOp &op) {
			const Purge *purge = std::get_if<Purge>(&op);
			return purge && purge->accountId == accountId;
		});
		if (purged) {
			continue;
		}
		std::optional<json> frame = FrameFromRow(stmt.get(), 0, 1, 2, 3);
		if (!frame) {
			continue;
		}
		const uint64_t seq = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 0));
		const std::string dest = ColumnText(stmt.get(), 4);
		const std::string msgId = ColumnText(stmt.get(), 5);
		const std::string authorId = ColumnText(stmt.get(), 6);
		const int64_t ts = sqlite3_column_int64(stmt.get(), 8);
		for (const PendingOp &op : pending) {
			const Redaction *redaction = std::get_if<Redaction>(&op);
			if (redaction && redaction->Matches(dest, seq, ts, msgId, authorId)) {
				RedactFrame(*frame, redaction->action);
			}
		}
		out.push_back(std::move(*frame));
	}
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		DBG(LogCat::Chat, "chat-archive: read failed: %s", sqlite3_errmsg(readDb_));
	}
	return out;
}

uint64_t ChatArchive::ReachBelow(uint64_t belowSeq) const
{
	const uint64_t foreign = foreignMax_.load(std::memory_order_acquire);
	return foreign > 0 ? std::max(belowSeq, foreign + 1) : belowSeq;
}

uint64_t ChatArchive::ClearedBelow(const std::deque<PendingOp> &pending) const
{
	uint64_t below = 0;
	for (const PendingOp &op : pending) {
		if (const Clear *clear = std::get_if<Clear>(&op)) {
			below = std::max(below, ReachBelow(clear->belowSeq));
		}
	}
	return below;
}

bool ChatArchive::Readable() const
{
	if (Active()) {
		return true;
	}
	std::lock_guard<std::mutex> lock(queueMutex_);
	return unknownSetting_ && !degraded_ && !stopped_;
}

ArchiveStatus ChatArchive::Status() const
{
	return ReportedStatus().first;
}

std::string ChatArchive::StatusDetail() const
{
	return ReportedStatus().second;
}

std::pair<ArchiveStatus, std::string> ChatArchive::ReportedStatus() const
{
	bool kept = false;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		kept = unreadableKept_;
	}
	// A kept file is named only while it is there: it may have been deleted since. Checked
	// outside the lock, and only while one is kept; a failed check counts as there.
	std::error_code ec;
	const bool there = kept && (fs::exists(fs::u8path(options_.path), ec) || ec);
	std::lock_guard<std::mutex> lock(queueMutex_);
	if (!there || !unreadableKept_) {
		return {status_, statusDetail_};
	}
	const std::string name = fs::u8path(options_.path).filename().u8string();
	// Degraded holds for the launch, as in SetStatus, so its detail names the file instead.
	if (degraded_) {
		return {status_, statusDetail_ + "; " + name + " could not be read and was left in place"};
	}
	return {ArchiveStatus::Unreadable, name};
}

json ChatArchive::StatusJson(std::chrono::milliseconds settle)
{
	json out{{"moderatedPlatforms", options_.moderatedPlatforms}};
	std::deque<PendingOp> pending;
	{
		std::unique_lock<std::mutex> lock(queueMutex_);
		const uint64_t queued = controlsQueued_;
		idle_.wait_for(lock, settle, [&] { return controlsDone_ >= queued; });
		pending = pending_;
	}
	const auto [status, detail] = ReportedStatus();
	out["status"] = StatusName(status);
	out["detail"] = detail;
	bool onDisk = false;
	if (!options_.path.empty()) {
		std::error_code ec;
		onDisk = fs::exists(fs::u8path(options_.path), ec) || ec;
		ForEachQuarantined([&](const fs::path &, const std::string &) { onDisk = true; });
	}
	out["onDisk"] = onDisk;
	int64_t rows = 0;
	{
		std::lock_guard<std::mutex> lock(readMutex_);
		if (readDb_) {
			// Rows a queued Clear has already hidden from readers are not counted either.
			Statement stmt(readDb_, "SELECT count(*) FROM messages WHERE seq >= ?1");
			if (stmt) {
				stmt.Bind(1, SqlSeq(ClearedBelow(pending)));
				if (stmt.Step() == SQLITE_ROW) {
					rows = sqlite3_column_int64(stmt.get(), 0);
				}
			}
		}
	}
	out["rows"] = rows;
	return out;
}

void ChatArchive::SetStatus(ArchiveStatus status, std::string detail)
{
	std::lock_guard<std::mutex> lock(queueMutex_);
	// Degraded holds for the rest of the launch, whatever the mode is changed to: nothing is
	// persisted again until the next one.
	if (degraded_) {
		return;
	}
	status_ = status;
	statusDetail_ = std::move(detail);
}

void ChatArchive::Shutdown()
{
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		stopped_ = true;
		held_ = false;
	}
	// Released before the join: the writer needs the lock to take its last batch.
	wake_.notify_all();
	if (writer_.joinable()) {
		writer_.join();
	}
	active_.store(false, std::memory_order_release);
	CloseStore();
	ReleaseLock();
}

void ChatArchive::HoldWrites(bool hold)
{
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		held_ = hold;
	}
	wake_.notify_all();
}

bool ChatArchive::WaitIdle(std::chrono::milliseconds timeout)
{
	std::unique_lock<std::mutex> lock(queueMutex_);
	return idle_.wait_for(lock, timeout, [this] { return ops_.empty() && !writing_; });
}

bool ChatArchive::WriterStarted() const
{
	std::lock_guard<std::mutex> lock(queueMutex_);
	return writer_.joinable();
}

void ChatArchive::WriterLoop()
{
	std::vector<Op> batch;
	std::unique_lock<std::mutex> lock(queueMutex_);
	for (;;) {
		const auto now = std::chrono::steady_clock::now();
		if (!stopped_) {
			if (held_) {
				wake_.wait(lock);
				continue;
			}
			if (ops_.empty()) {
				if (!db_.IsOpen()) {
					wake_.wait(lock);
				} else if (now < nextSweep_) {
					wake_.wait_until(lock, nextSweep_);
				} else {
					lock.unlock();
					try {
						Sweep();
					} catch (const std::exception &e) {
						DegradeFromWriter(std::string("unexpected error: ") + e.what());
					}
					lock.lock();
				}
				continue;
			}
			const auto due = firstQueuedAt_ + kBatchDelay;
			if (!urgent_ && ops_.size() < kBatchRows && now < due) {
				wake_.wait_until(lock, due);
				continue;
			}
		} else if (ops_.empty()) {
			break;
		}
		batch.assign(std::make_move_iterator(ops_.begin()), std::make_move_iterator(ops_.end()));
		ops_.clear();
		urgent_ = false;
		writing_ = true;
		const auto controls = static_cast<uint64_t>(std::count_if(batch.begin(), batch.end(), [](const Op &op) {
			return !std::holds_alternative<Row>(op);
		}));
		lock.unlock();

		// An exception escaping this thread would end the process.
		try {
			Apply(batch);
			if (db_.IsOpen() && (std::chrono::steady_clock::now() >= nextSweep_ || count_ > kSweepAtRows)) {
				Sweep();
			}
		} catch (const std::exception &e) {
			DegradeFromWriter(std::string("unexpected error: ") + e.what());
		}
		batch.clear();

		lock.lock();
		writing_ = false;
		controlsDone_ += controls;
		idle_.notify_all();
	}
}

void ChatArchive::Apply(std::vector<Op> &batch)
{
	// A mode change splits the batch: what was queued before it is written under the old
	// mode, and what follows under the new one.
	std::vector<Op> run;
	for (Op &op : batch) {
		if (const SetMode *mode = std::get_if<SetMode>(&op)) {
			Commit(run);
			run.clear();
			ApplyMode(mode->retention);
			continue;
		}
		run.push_back(std::move(op));
	}
	Commit(run);
}

bool ChatArchive::Commit(std::vector<Op> &ops, bool queued)
{
	if (ops.empty()) {
		return true;
	}
	size_t pendingCount = 0;
	bool dropsHistory = false; // a Clear or a purge, which also owe the quarantined copies
	for (const Op &op : ops) {
		const bool drops = std::holds_alternative<Clear>(op) || std::holds_alternative<Purge>(op);
		dropsHistory = dropsHistory || drops;
		pendingCount += drops || std::holds_alternative<Redaction>(op) ? 1 : 0;
	}
	bool degraded = false;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		degraded = degraded_;
	}
	if (degraded) {
		ops.erase(std::remove_if(ops.begin(), ops.end(),
					 [](const Op &op) { return std::holds_alternative<Row>(op); }),
			  ops.end());
	}

	bool committed = true;
	if (db_.IsOpen() && !ops.empty()) {
		int64_t countDelta = 0;
		uint64_t maxSeq = 0;
		bool rewrote = false;
		// The self-test's failure stands for one that outlasted every retry.
		const bool failing = failWrites_.load(std::memory_order_acquire);
		committed = !failing && TryCommit(ops, countDelta, maxSeq, rewrote);
		if (failing) {
			writeError_ = "self-test";
		}
		for (size_t attempt = 0; !committed && !failing && attempt < std::size(kRetryBackoff); ++attempt) {
			DBG(LogCat::Chat, "chat-archive: commit failed (%s); retrying", writeError_.c_str());
			std::this_thread::sleep_for(kRetryBackoff[attempt]);
			countDelta = 0;
			maxSeq = 0;
			rewrote = false;
			committed = TryCommit(ops, countDelta, maxSeq, rewrote);
		}
		if (committed) {
			count_ += countDelta;
			RaiseTo(persistedSeq_, maxSeq);
			if (rewrote) {
				Checkpoint();
			}
		} else {
			DegradeFromWriter("chat.db write failed: " + writeError_);
		}
	} else if (!db_.IsOpen() && !degraded) {
		bool overflowed = false;
		for (const Op &op : ops) {
			overflowed = KeepMissed(op) || overflowed;
		}
		if (overflowed) {
			LogMissedOverflow();
		}
	}
	// Off: everything this build may remove goes, the quarantined copies with it, and so,
	// whatever the mode, does a store an earlier try left owing. Otherwise a Clear or a
	// purge still takes the quarantined copies.
	if (!RemoveWhatIsOwed() && dropsHistory) {
		WithStoreLock([this] { DeleteQuarantined(true); });
	}
	if (queued && pendingCount > 0) {
		std::lock_guard<std::mutex> lock(queueMutex_);
		pending_.erase(pending_.begin(),
			       pending_.begin() + static_cast<std::ptrdiff_t>(std::min(pendingCount, pending_.size())));
	}
	return committed;
}

bool ChatArchive::TryCommit(const std::vector<Op> &ops, int64_t &countDelta, uint64_t &maxSeq, bool &rewrote)
{
	sqlite3 *db = db_.Handle();
	if (!db_.Exec("BEGIN IMMEDIATE")) {
		writeError_ = db_.LastError();
		return false;
	}
	const auto fail = [&] {
		writeError_ = sqlite3_errmsg(db);
		db_.Exec("ROLLBACK");
		return false;
	};
	// A statement that fails for one row only: skip the row, log it without its text.
	const auto rowFailed = [&](int rc, const char *what, uint64_t seq) {
		if (IsStoreFailure(rc)) {
			return true;
		}
		HostLog(std::string("[chat-archive] skipped a row that could not be ") + what + " (seq " +
			std::to_string(seq) + "): " + sqlite3_errstr(rc));
		return false;
	};
	const uint64_t foreignMax = foreignMax_.load(std::memory_order_relaxed); // the writer's own
	for (const Op &op : ops) {
		if (const Row *row = std::get_if<Row>(&op)) {
			if (row->seq <= foreignMax) {
				// Admitted before the store was adopted, with a seq among the rows another
				// launch stored: it would take one's key, or sit inside that launch's order.
				// It stays in the ring only, like a row admitted before persistence came on.
				maxSeq = std::max(maxSeq, row->seq);
				continue;
			}
			sqlite3_reset(insert_);
			sqlite3_clear_bindings(insert_);
			sqlite3_bind_int64(insert_, 1, SqlSeq(row->seq));
			const auto text = [&](int index, const std::string &value) {
				sqlite3_bind_text(insert_, index, value.data(), static_cast<int>(value.size()),
						  SQLITE_STATIC);
			};
			text(2, row->dest);
			text(3, row->msgId);
			text(4, row->platform);
			text(5, row->accountId);
			text(6, row->profileUuid);
			text(7, row->authorId);
			sqlite3_bind_int64(insert_, 8, row->ts);
			sqlite3_bind_int64(insert_, 9, row->rx);
			text(10, options_.launchId);
			text(11, row->body);
			const int rc = sqlite3_step(insert_);
			sqlite3_reset(insert_);
			if (rc == SQLITE_DONE) {
				countDelta += sqlite3_changes(db);
			} else if (rowFailed(rc, "stored", row->seq)) {
				return fail();
			}
			// Settled either way: a skipped row will never be written.
			maxSeq = std::max(maxSeq, row->seq);
		} else if (const Redaction *redaction = std::get_if<Redaction>(&op)) {
			// The same guard Redaction::Matches keeps: an op naming no message or no
			// author removes nothing, rather than every row with an empty column.
			if ((redaction->action == ModerationAction::Delete && redaction->msgId.empty()) ||
			    (redaction->action == ModerationAction::ClearUser && redaction->authorId.empty())) {
				continue;
			}
			// Queue order already keeps rows admitted after the op out of reach; the seq
			// bound states it in the query as Redaction::Matches does, lifted over the rows
			// of a store adopted after the op was issued (ReachBelow).
			std::string sql = "SELECT seq, body FROM messages WHERE dest = ?1 AND seq < ?2";
			if (redaction->action == ModerationAction::Delete) {
				sql += " AND msg_id = ?3";
			} else if (redaction->action == ModerationAction::ClearUser) {
				sql += " AND author_id = ?3";
			}
			if (redaction->beforeTs) {
				sql += " AND ts > 0 AND ts <= ?4"; // Redaction::Matches' time bound
			}
			Statement select(db, sql);
			if (!select) {
				return fail();
			}
			select.Bind(1, redaction->dest);
			select.Bind(2, SqlSeq(ReachBelow(redaction->belowSeq)));
			if (redaction->action != ModerationAction::ClearAll) {
				select.Bind(3, redaction->action == ModerationAction::Delete ? redaction->msgId
											     : redaction->authorId);
			}
			if (redaction->beforeTs) {
				select.Bind(4, *redaction->beforeTs);
			}
			std::vector<std::pair<int64_t, std::string>> hits;
			int rc = SQLITE_ROW;
			while ((rc = select.Step()) == SQLITE_ROW) {
				hits.emplace_back(sqlite3_column_int64(select.get(), 0), ColumnText(select.get(), 1));
			}
			if (rc != SQLITE_DONE) {
				return fail();
			}
			for (auto &[seq, body] : hits) {
				json frame = JsonUtil::ParseJson(body);
				// A body that no longer parses cannot be redacted, so the row goes.
				Statement write(db,
						frame.is_object()
							? "UPDATE messages SET deleted = ?1, body = ?2 WHERE seq = ?3"
							: "DELETE FROM messages WHERE seq = ?3");
				if (!write) {
					return fail();
				}
				if (frame.is_object()) {
					RedactFrame(frame, redaction->action);
					write.Bind(1, std::string(DeletedMark(redaction->action)));
					write.Bind(2, JsonUtil::DumpLossy(frame));
				}
				write.Bind(3, seq);
				const int wrc = write.Step();
				if (wrc != SQLITE_DONE && rowFailed(wrc, "redacted", static_cast<uint64_t>(seq))) {
					return fail();
				}
				if (!frame.is_object()) {
					countDelta -= sqlite3_changes(db);
				}
				rewrote = true;
			}
		} else if (const Clear *clear = std::get_if<Clear>(&op)) {
			Statement del(db, clear->receivedBy ? "DELETE FROM messages WHERE rx <= ?1"
							    : "DELETE FROM messages WHERE seq < ?1");
			if (!del) {
				return fail();
			}
			del.Bind(1, clear->receivedBy ? *clear->receivedBy : SqlSeq(ReachBelow(clear->belowSeq)));
			if (del.Step() != SQLITE_DONE) {
				return fail();
			}
			countDelta -= sqlite3_changes(db);
			rewrote = true;
		} else if (const Purge *purge = std::get_if<Purge>(&op)) {
			Statement del(db, "DELETE FROM messages WHERE account_id = ?1");
			if (!del) {
				return fail();
			}
			del.Bind(1, purge->accountId);
			if (del.Step() != SQLITE_DONE) {
				return fail();
			}
			countDelta -= sqlite3_changes(db);
			rewrote = true;
		}
	}
	if (!db_.Exec("COMMIT")) {
		return fail();
	}
	return true;
}

void ChatArchive::ApplyMode(Retention retention)
{
	const Retention before = retention_;
	retention_ = retention;
	if (retention == Retention::Off) {
		if (db_.IsOpen()) {
			db_.Exec("DELETE FROM messages");
			count_ = 0;
			Checkpoint();
			CloseStore();
		}
		sessionKeepsThrough_ = 0;
		SetStatus(ArchiveStatus::Off, {});
		RemoveOrOwe(kOffReason, true);
		ReleaseLock();
		return;
	}
	if (retention == Retention::Session && before == Retention::SevenDays) {
		// The one move to This session the user confirms: it deletes what earlier launches
		// stored, what a late open kept included.
		sessionKeepsThrough_ = 0;
	}
	if (!db_.IsOpen()) {
		if (IsDegraded()) {
			// Nothing is written again this launch, so nothing is opened to write to; a
			// store a failed writer left behind is still owed its removal.
			RemoveWhatIsOwed();
			return;
		}
		if (!OpenStore(false)) {
			StopPersisting();
			// The copies set aside still age out, and This session still removes them all.
			WithStoreLock([this] { SweepQuarantined(); });
			if (retention == Retention::Session && before == Retention::SevenDays) {
				// The user confirmed deleting what earlier launches stored; a store that
				// opens later (Session to 7 days, say) must not bring it back.
				SetOwedRemoval(kSessionReason);
			}
			return;
		}
		if (!AdoptStore(before)) {
			StopPersisting();
		}
		return;
	}
	Sweep();
}

bool ChatArchive::AdoptStore(Retention before)
{
	// The store was not open when this launch seeded its seqs: another instance held it, or
	// Off had closed it. What it holds is kept, since moving to a keeping mode deletes
	// nothing the user was not asked about, unless a removal the user confirmed this launch
	// (Off, or 7 days to This session) is still owed.
	const std::string owed = OwedRemoval();
	if (!owed.empty() && count_ > 0) {
		HostLog("[chat-archive] removing " + std::to_string(count_) + " row(s) (" + owed + ")");
		if (!db_.Exec("DELETE FROM messages")) {
			DegradeFromWriter("chat.db write failed: " + db_.LastError());
			return GiveUpAdoption(owed);
		}
		count_ = 0;
		Checkpoint();
	}
	SetOwedRemoval({}); // the file a removal could not reach is the live store again
	const auto storedMax = static_cast<uint64_t>(
		std::max<int64_t>(0, db_.ScalarInt("SELECT coalesce(max(seq), 0) FROM messages")));
	// Every seq bound issued before this point now reaches the stored rows too (ReachBelow),
	// and a row queued with a seq among them is not written (TryCommit).
	foreignMax_.store(storedMax, std::memory_order_release);
	if (retention_ == Retention::Session && before != Retention::SevenDays) {
		// Moving to This session from Off asks nothing, so it deletes nothing: what another
		// instance stored stays for this launch, still under the age and platform limits and
		// Clear, and goes at the next launch's open, as an earlier launch's rows do.
		sessionKeepsThrough_ = storedMax;
	}
	// What this launch removed while no store was open reaches the one it now has. The rows
	// are kept only if it does: otherwise the store goes, as the removals would have it.
	std::vector<Op> missed;
	if (missed_.clearedAt) {
		missed.push_back(Clear{0, missed_.clearedAt});
	}
	for (std::string &accountId : missed_.purgedAccounts) {
		missed.push_back(Purge{std::move(accountId)});
	}
	for (Redaction &redaction : missed_.redactions) {
		missed.push_back(std::move(redaction));
	}
	missed_ = {};
	if (!Commit(missed, false)) {
		return GiveUpAdoption(kAbandonedReason);
	}
	if (raiseSeq_) {
		raiseSeq_(storedMax);
	}
	// Nothing is read before retention has run: the age and row limits, and an earlier
	// launch's rows on a platform whose moderation is not honored.
	if (!Sweep()) {
		DegradeFromWriter("chat.db retention sweep failed");
		return GiveUpAdoption({});
	}
	if (count_ > 0) {
		HostLog("[chat-archive] kept " + std::to_string(count_) +
			" row(s) this launch did not start from; its seqs continue above " + std::to_string(storedMax));
	}
	return OpenReader();
}

bool ChatArchive::GiveUpAdoption(const std::string &removal)
{
	CloseStore();
	if (!removal.empty()) {
		RemoveOrOwe(removal, true);
	}
	ReleaseLock();
	return false;
}

bool ChatArchive::KeepMissed(const Op &op)
{
	if (std::holds_alternative<Clear>(op)) {
		missed_.clearedAt = TimeUtil::NowMs();
	} else if (const Purge *purge = std::get_if<Purge>(&op)) {
		std::vector<std::string> &accounts = missed_.purgedAccounts;
		if (std::find(accounts.begin(), accounts.end(), purge->accountId) == accounts.end()) {
			accounts.push_back(purge->accountId);
		}
	} else if (const Redaction *redaction = std::get_if<Redaction>(&op)) {
		Redaction kept = *redaction;
		// Replayed later, an author-wide removal is bounded by the time it was seen, as one
		// read out of a platform's history is (SafeToReplay): it must not reach what the
		// author said after it, which the store may hold.
		if (kept.action != ModerationAction::Delete && !kept.beforeTs) {
			kept.beforeTs = TimeUtil::NowMs();
		}
		bool overflowed = false;
		if (missed_.redactions.size() >= kMaxMissedRedactions) {
			missed_.redactions.pop_front();
			overflowed = !missed_.overflowed;
			missed_.overflowed = true;
		}
		missed_.redactions.push_back(std::move(kept));
		return overflowed;
	}
	return false;
}

void ChatArchive::LogMissedOverflow()
{
	HostLog("[chat-archive] more than " + std::to_string(kMaxMissedRedactions) +
		" moderator removals while chat.db was closed; the oldest will not reach it if it opens later");
}

bool ChatArchive::Sweep()
{
	nextSweep_ = std::chrono::steady_clock::now() + kSweepInterval;
	SweepQuarantined();
	if (!db_.IsOpen()) {
		return true;
	}
	sqlite3 *db = db_.Handle();
	const bool failing = failWrites_.load(std::memory_order_acquire); // the self-test's failure
	if (failing || !db_.Exec("BEGIN IMMEDIATE")) {
		HostLog("[chat-archive] retention sweep could not start: " +
			(failing ? std::string("self-test") : db_.LastError()));
		return false;
	}
	int64_t removed = 0;
	bool ok = true;
	const auto run = [&](const std::string &sql, const std::function<void(Statement &)> &bind) {
		if (!ok) {
			return;
		}
		Statement stmt(db, sql);
		if (!stmt) {
			ok = false;
			return;
		}
		bind(stmt);
		if (stmt.Step() != SQLITE_DONE) {
			ok = false;
			return;
		}
		removed += sqlite3_changes(db);
	};
	run("DELETE FROM messages WHERE rx < ?1", [&](Statement &s) { s.Bind(1, TimeUtil::NowMs() - kRetentionMs); });
	// Rows of an earlier launch survive only on a platform whose moderation is honored.
	std::string sql = "DELETE FROM messages WHERE launch_id != ?1";
	if (!options_.moderatedPlatforms.empty()) {
		sql += " AND platform NOT IN (";
		for (size_t i = 0; i < options_.moderatedPlatforms.size(); ++i) {
			sql += (i ? ", ?" : "?") + std::to_string(i + 2);
		}
		sql += ")";
	}
	run(sql, [&](Statement &s) {
		s.Bind(1, options_.launchId);
		for (size_t i = 0; i < options_.moderatedPlatforms.size(); ++i) {
			s.Bind(static_cast<int>(i + 2), options_.moderatedPlatforms[i]);
		}
	});
	if (retention_ == Retention::Session) {
		// Under This session they survive on no platform, bar what a late open kept.
		run("DELETE FROM messages WHERE launch_id != ?1 AND seq > ?2", [&](Statement &s) {
			s.Bind(1, options_.launchId);
			s.Bind(2, SqlSeq(sessionKeepsThrough_));
		});
	}
	run("DELETE FROM messages WHERE seq < (SELECT seq FROM messages ORDER BY seq DESC LIMIT 1 OFFSET ?1)",
	    [&](Statement &s) { s.Bind(1, kMaxRows - 1); });
	if (!ok || !db_.Exec("COMMIT")) {
		const std::string cause = ok ? db_.LastError() : sqlite3_errmsg(db);
		db_.Exec("ROLLBACK");
		HostLog("[chat-archive] retention sweep failed: " + cause);
		return false;
	}
	count_ -= removed;
	if (removed > 0) {
		DBG(LogCat::Chat, "chat-archive: retention removed %lld row(s)", static_cast<long long>(removed));
		Checkpoint();
	}
	return true;
}

void ChatArchive::Checkpoint()
{
	// TRUNCATE also empties the WAL, which still holds the removed rows' old pages;
	// secure_delete only scrubs the main file.
	for (int attempt = 0; attempt < kCheckpointAttempts; ++attempt) {
		const int rc =
			sqlite3_wal_checkpoint_v2(db_.Handle(), nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
		if (rc == SQLITE_OK) {
			return;
		}
		if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
			HostLog(std::string("[chat-archive] checkpoint failed: ") + sqlite3_errstr(rc));
			return;
		}
		std::this_thread::sleep_for(kCheckpointPause);
	}
	DBG(LogCat::Chat, "chat-archive: checkpoint busy; the next one truncates the WAL");
}

bool ChatArchive::OpenStore(bool publishReader)
{
	if (!AcquireLock()) {
		SetStatus(ArchiveStatus::Disabled, "chat.db is in use by another Braidcast");
		return false;
	}

	bool opened = db_.Open(options_.path, kChatLadder);
	if (!opened && db_.NewerSchema()) {
		return GiveUpStore(ArchiveStatus::NewerSchema, db_.LastError());
	}
	bool corrupt = !opened && IsCorruptFile(db_.LastErrorCode());
	if (opened) {
		Statement check(db_.Handle(), "PRAGMA quick_check");
		const std::string verdict = check && check.Step() == SQLITE_ROW ? ColumnText(check.get(), 0) : "";
		if (verdict != "ok") {
			HostLog("[chat-archive] chat.db failed its integrity check: " +
				(verdict.empty() ? db_.LastError() : verdict));
			db_.Close();
			opened = false;
			corrupt = true;
		}
	}
	// Recovered names the copy it set aside in its detail; in Session mode the unreadable
	// file is deleted instead, and the detail is empty.
	bool recovered = false;
	std::string setAside;
	if (!opened) {
		if (!corrupt) {
			return GiveUpStore(ArchiveStatus::Disabled, db_.LastError());
		}
		if (retention_ == Retention::Session) {
			recovered = DiscardCorrupt();
		} else {
			setAside = Quarantine();
			recovered = !setAside.empty();
		}
		if (!recovered) {
			return GiveUpStore(ArchiveStatus::Disabled,
					   "chat.db is unreadable and could not be moved out of the way");
		}
		if (!db_.Open(options_.path, kChatLadder)) {
			return GiveUpStore(ArchiveStatus::Disabled, db_.LastError());
		}
	}
	// Best effort: deleted and redacted rows are overwritten in the main file, while the
	// WAL and the disk's own remapping are outside its reach.
	db_.Exec("PRAGMA secure_delete = ON");
	if (sqlite3_prepare_v2(db_.Handle(),
			       "INSERT OR IGNORE INTO messages (seq, dest, msg_id, platform, account_id, profile_uuid, "
			       "author_id, ts, rx, launch_id, deleted, body) "
			       "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, '', ?11)",
			       -1, &insert_, nullptr) != SQLITE_OK) {
		return GiveUpStore(ArchiveStatus::Disabled, sqlite3_errmsg(db_.Handle()));
	}
	count_ = db_.ScalarInt("SELECT count(*) FROM messages");
	SetStatus(recovered ? ArchiveStatus::Recovered : ArchiveStatus::Ok, setAside);
	return !publishReader || OpenReader();
}

bool ChatArchive::OpenReader()
{
	sqlite3 *reader = nullptr;
	if (sqlite3_open_v2(options_.path.c_str(), &reader, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) !=
	    SQLITE_OK) {
		const std::string cause = reader ? sqlite3_errmsg(reader) : "out of memory";
		sqlite3_close_v2(reader);
		return GiveUpStore(ArchiveStatus::Disabled, cause);
	}
	sqlite3_busy_timeout(reader, kBusyTimeoutMs);
	sqlite3_exec(reader, "PRAGMA query_only = ON", nullptr, nullptr, nullptr);
	std::lock_guard<std::mutex> lock(readMutex_);
	readDb_ = reader;
	return true;
}

bool ChatArchive::GiveUpStore(ArchiveStatus status, std::string detail)
{
	SetStatus(status, std::move(detail));
	CloseStore();
	ReleaseLock();
	return false;
}

void ChatArchive::CloseStore()
{
	if (insert_) {
		sqlite3_finalize(insert_);
		insert_ = nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(readMutex_);
		if (readDb_) {
			sqlite3_close_v2(readDb_);
			readDb_ = nullptr;
		}
	}
	db_.Close();
}

bool ChatArchive::AcquireLock()
{
	if (lock_) {
		return true;
	}
	// Opened without sharing, so a second instance on the same data directory that got
	// past the single-instance guard (another logon session, an older build, a dangling
	// junction) fails here. The file goes with the handle, a crash included.
	const std::wstring path = Encoding::Utf8ToWide(options_.path + kLockSuffix);
	HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
				    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return false;
	}
	lock_ = handle;
	return true;
}

void ChatArchive::ReleaseLock()
{
	if (lock_) {
		CloseHandle(static_cast<HANDLE>(lock_));
		lock_ = nullptr;
	}
}

void ChatArchive::ForEachQuarantined(const std::function<void(const fs::path &, const std::string &)> &visit) const
{
	const fs::path db = fs::u8path(options_.path);
	const std::string prefix = db.filename().u8string() + kQuarantineInfix;
	std::error_code ec;
	std::vector<std::pair<fs::path, std::string>> found;
	for (fs::directory_iterator it(db.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
		const std::string name = it->path().filename().u8string();
		if (name.rfind(prefix, 0) == 0) {
			found.emplace_back(it->path(), name.substr(prefix.size()));
		}
	}
	for (const auto &[path, stamp] : found) {
		visit(path, stamp);
	}
}

void ChatArchive::SweepQuarantined()
{
	DeleteQuarantined(retention_ == Retention::Session);
}

bool ChatArchive::WithStoreLock(const std::function<void()> &fn)
{
	const bool held = lock_ != nullptr;
	if (!AcquireLock()) {
		return false;
	}
	fn();
	if (!held) {
		ReleaseLock();
	}
	return true;
}

void ChatArchive::DeleteQuarantined(bool all)
{
	const std::time_t now = std::time(nullptr);
	int removed = 0;
	std::vector<std::string> names;
	ForEachQuarantined([&](const fs::path &path, const std::string &stamp) {
		// A name this archive did not stamp still carries its prefix, so it is its copy.
		const std::optional<std::time_t> at = QuarantineTime(stamp);
		if (!all && at && (now - *at) * 1000 < kRetentionMs) {
			return;
		}
		std::error_code ec;
		if (fs::remove(path, ec)) {
			++removed;
			names.push_back(path.filename().u8string());
		} else if (ec) {
			HostLog("[chat-archive] could not delete " + path.filename().u8string() + ": " + ec.message());
		}
	});
	if (removed > 0) {
		HostLog("[chat-archive] deleted " + std::to_string(removed) + " quarantined chat.db file(s)");
		// Recovered names the copy it set aside; once that copy is gone it is plain Ok.
		std::lock_guard<std::mutex> lock(queueMutex_);
		if (status_ == ArchiveStatus::Recovered &&
		    std::find(names.begin(), names.end(), statusDetail_) != names.end()) {
			status_ = ArchiveStatus::Ok;
			statusDetail_.clear();
		}
	}
}

std::string ChatArchive::Quarantine()
{
	const fs::path db = fs::u8path(options_.path);
	const fs::path dir = db.parent_path();
	const std::string base = db.filename().u8string() + kQuarantineInfix + TimeUtil::LocalFileStamp();
	std::error_code ec;
	const auto taken = [&](const std::string &name) {
		if (fs::exists(dir / fs::u8path(name), ec)) {
			return true;
		}
		for (const char *side : kSideFiles) {
			if (fs::exists(dir / fs::u8path(name + side), ec)) {
				return true;
			}
		}
		return false;
	};
	std::string name = base;
	for (int n = 2; taken(name); ++n) {
		name = base + "-" + std::to_string(n);
	}
	// The -wal goes first: one left beside the fresh file would be replayed into it. A
	// missing -wal or -shm is normal (a clean close removes both).
	for (const char *side : kSideFiles) {
		const fs::path from = fs::u8path(options_.path + side);
		if (!fs::exists(from, ec)) {
			continue;
		}
		fs::rename(from, dir / fs::u8path(name + side), ec);
		if (ec) {
			HostLog("[chat-archive] could not set aside chat.db" + std::string(side) + ": " + ec.message());
			return {};
		}
	}
	fs::rename(db, dir / fs::u8path(name), ec);
	if (ec) {
		HostLog("[chat-archive] could not set aside chat.db: " + ec.message());
		return {};
	}
	HostLog("[chat-archive] chat.db was unreadable; kept as " + name + " and started a fresh one");
	return name;
}

bool ChatArchive::RemoveStoreFiles(const std::string &why, bool countLocked, bool &unreadable)
{
	std::error_code failed;
	const auto there = [&](const fs::path &path) {
		std::error_code ec;
		const bool exists = fs::exists(path, ec);
		if (ec && !failed) {
			failed = ec;
		}
		return exists;
	};
	bool anyStore = there(fs::u8path(options_.path));
	for (const char *side : kSideFiles) {
		anyStore = there(fs::u8path(options_.path + side)) || anyStore;
	}
	if (failed) {
		// Nothing is known to be gone, so nothing was tried: the removal stays owed.
		HostLog("[chat-archive] could not check for chat.db (" + failed.message() + "); left for another try");
		return true;
	}
	bool anyQuarantined = false;
	ForEachQuarantined([&](const fs::path &, const std::string &) { anyQuarantined = true; });
	if (!anyStore && !anyQuarantined) {
		unreadable = false;
		return false;
	}
	bool left = false;
	const bool locked = WithStoreLock([&] {
		unreadable = false;
		if (anyStore) {
			left = RemoveOwnStore(why, unreadable);
		}
		DeleteQuarantined(true);
	});
	if (!locked) {
		HostLog("[chat-archive] chat.db is in use by another Braidcast; left in place");
		return countLocked;
	}
	return left;
}

bool ChatArchive::RemoveOwnStore(const std::string &why, bool &unreadable)
{
	// A newer build's file is left exactly as it is, whatever this build's mode, and so is
	// one whose version cannot be read: a failed probe proves nothing about it. Only a file
	// SQLite rejects as not a database at all is known not to be a newer build's.
	unreadable = false;
	std::error_code ec;
	if (!fs::exists(fs::u8path(options_.path), ec) && !ec) {
		// A -wal or -shm without its database holds nothing to read.
		return !DeleteStoreFiles();
	}
	sqlite3 *probe = nullptr;
	int rc = sqlite3_open_v2(options_.path.c_str(), &probe, SQLITE_OPEN_READONLY, nullptr);
	std::optional<int> version;
	if (rc == SQLITE_OK) {
		Statement stmt(probe, "PRAGMA user_version");
		rc = stmt ? stmt.Step() : sqlite3_errcode(probe);
		if (rc == SQLITE_ROW) {
			version = sqlite3_column_int(stmt.get(), 0);
		}
	}
	const std::string cause = probe ? sqlite3_errmsg(probe) : sqlite3_errstr(rc);
	sqlite3_close_v2(probe);
	if (version && *version > kChatLadder.Current()) {
		HostLog("[chat-archive] chat.db belongs to a newer build; left in place");
		return false;
	}
	if (!version && !IsCorruptFile(rc)) {
		HostLog("[chat-archive] chat.db could not be read to check its version (" + cause + "); left in place");
		unreadable = true;
		return true;
	}
	if (!DeleteStoreFiles()) {
		return true;
	}
	HostLog("[chat-archive] removed stored chat history (" + why + ")");
	return false;
}

bool ChatArchive::DeleteStoreFiles()
{
	// The -wal first: one left beside a fresh chat.db would be replayed into it.
	std::error_code ec;
	for (const char *side : kSideFiles) {
		fs::remove(fs::u8path(options_.path + side), ec);
		if (ec) {
			HostLog("[chat-archive] could not delete chat.db" + std::string(side) + ": " + ec.message());
			return false;
		}
	}
	fs::remove(fs::u8path(options_.path), ec);
	if (ec) {
		HostLog("[chat-archive] could not delete chat.db: " + ec.message());
		return false;
	}
	return true;
}

bool ChatArchive::DiscardCorrupt()
{
	if (!DeleteStoreFiles()) {
		return false;
	}
	HostLog("[chat-archive] chat.db was unreadable; deleted it, since chat history is kept for this session "
		"only, and started a fresh one");
	return true;
}

void ChatArchive::BuildSeed(Seed &seed)
{
	Statement stmt(db_.Handle(), "SELECT seq, rx, deleted, body, dest FROM messages ORDER BY seq DESC LIMIT ?1");
	if (!stmt) {
		return;
	}
	stmt.Bind(1, static_cast<int64_t>(ChatHistory::kCap));
	uint64_t newest = 0;
	while (stmt.Step() == SQLITE_ROW) {
		newest = std::max(newest, static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 0)));
		if (std::optional<json> frame = FrameFromRow(stmt.get(), 0, 1, 2, 3)) {
			seed.rows.push_back({ColumnText(stmt.get(), 4), std::move(*frame)});
		}
	}
	std::reverse(seed.rows.begin(), seed.rows.end());
	seed.nextSeq = newest + 1;
}

ChatArchive &Archive()
{
	static ChatArchive archive;
	return archive;
}

} // namespace Chat
