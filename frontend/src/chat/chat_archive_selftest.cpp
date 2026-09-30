// RunChatArchiveSelfTest (declared in obs_bootstrap.hpp with the rest of the smoke battery).
// Every case runs on a private ChatArchive in a scratch directory under %TEMP%, never on
// Chat::Archive() or the user's chat.db.

#include "../obs_bootstrap.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "../log.hpp"
#include "../util/file_util.hpp"
#include "../util/random_util.hpp"
#include "../util/time_util.hpp"
#include "chat_archive.hpp"
#include "chat_history.hpp"

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono_literals;

constexpr auto kIdleWait = 10s;

const OAuth::DestinationId kTwitch{"twitch:1", ""};
const OAuth::DestinationId kKickA{"kick:a", ""};
const OAuth::DestinationId kKickB{"kick:b", ""};

void Report(const std::string &name, bool ok, const std::string &detail = {})
{
	HostLog("[selftest] chat-archive " + name + " -> " + (ok ? "OK" : "FAIL") +
		(detail.empty() ? std::string() : " (" + detail + ")"));
}

// A chat.message frame as the hub stamps it: identity, destination, author, some text.
json Frame(const OAuth::DestinationId &dest, const std::string &id, const std::string &platform)
{
	json frame{{"id", id},
		   {"platform", platform},
		   {"ts", 1000},
		   {"author", {{"name", "viewer"}, {"id", "author-" + id}, {"color", ""}, {"badges", json::array()}}},
		   {"fragments", json::array({{{"type", "text"}, {"text", "hello " + id}}})},
		   {"accountId", dest.accountId}};
	if (!dest.profileUuid.empty()) {
		frame["profileUuid"] = dest.profileUuid;
	}
	return frame;
}

bool Add(Chat::ChatHistory &history, const OAuth::DestinationId &dest, const std::string &id,
	 const std::string &platform = "twitch")
{
	json frame = Frame(dest, id, platform);
	return history.Add(dest, frame);
}

// A kick message from `authorId`, said at platform time `ts`.
bool AddFrom(Chat::ChatHistory &history, const OAuth::DestinationId &dest, const std::string &id,
	     const std::string &authorId, int64_t ts)
{
	json frame = Frame(dest, id, "kick");
	frame["author"]["id"] = authorId;
	frame["ts"] = ts;
	return history.Add(dest, frame);
}

// Every message a dock can reach, walked page by page from the newest back to the start
// the way the dock does. Returned oldest-first; `ordered` is false when a page repeats a
// seq or goes backwards, and `unreadable` is set when the walk stopped at a page that
// could not say whether older messages exist.
struct Walk {
	std::vector<std::string> ids;
	std::vector<uint64_t> seqs;
	bool ordered = true;
	bool unreadable = false;
};

Walk WalkAll(const Chat::ChatHistory &history, size_t limit = 50, const Feed::Filter &filter = Feed::Filter{})
{
	Walk walk;
	std::vector<json> newestFirst;
	std::optional<uint64_t> before;
	for (;;) {
		const Chat::ChatPage page = history.Page(before, limit, filter);
		for (auto it = page.items.rbegin(); it != page.items.rend(); ++it) {
			newestFirst.push_back(*it);
		}
		if (page.unreadable) {
			walk.unreadable = true;
			break;
		}
		if (!page.more || page.items.empty()) {
			break;
		}
		before = page.items.front().value("seq", uint64_t(0));
	}
	for (auto it = newestFirst.rbegin(); it != newestFirst.rend(); ++it) {
		const uint64_t seq = it->value("seq", uint64_t(0));
		if (!walk.seqs.empty() && seq <= walk.seqs.back()) {
			walk.ordered = false;
		}
		walk.seqs.push_back(seq);
		walk.ids.push_back(it->value("id", std::string()));
	}
	return walk;
}

std::string Joined(const std::vector<std::string> &ids)
{
	std::string out;
	for (const std::string &id : ids) {
		out += (out.empty() ? "" : ",") + id;
	}
	return out;
}

// Straight SQL against a chat.db, beside the archive's own connections.
int64_t Scalar(const std::string &path, const std::string &sql)
{
	sqlite3 *db = nullptr;
	int64_t value = -1;
	if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK) {
		sqlite3_busy_timeout(db, 3000);
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
			const int rc = sqlite3_step(stmt);
			value = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : rc == SQLITE_DONE ? 0 : -1;
		}
		sqlite3_finalize(stmt);
	}
	sqlite3_close_v2(db);
	return value;
}

std::string Column(const std::string &path, const std::string &sql)
{
	sqlite3 *db = nullptr;
	std::string out;
	if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const unsigned char *text = sqlite3_column_text(stmt, 0);
				out += (out.empty() ? "" : ",") +
				       std::string(text ? reinterpret_cast<const char *>(text) : "");
			}
		}
		sqlite3_finalize(stmt);
	}
	sqlite3_close_v2(db);
	return out;
}

bool Exists(const std::string &path)
{
	std::error_code ec;
	return fs::exists(fs::u8path(path), ec);
}

std::string Bytes(const std::string &path)
{
	std::string out;
	FileUtil::ReadUtf8File(path, out);
	return out;
}

// A WAL-mode database at schema `version`: 99 is a newer build's, past this build's ladder.
void WriteWalDb(const std::string &path, int version)
{
	sqlite3 *db = nullptr;
	sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
	const std::string sql = "PRAGMA journal_mode = WAL; CREATE TABLE future (x); INSERT INTO future VALUES (1);"
				"PRAGMA user_version = " +
				std::to_string(version) + ";";
	sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
	sqlite3_close_v2(db);
}

constexpr int kNewerVersion = 99;

// 8 KB that is not a database.
std::string Garbage()
{
	std::string garbage;
	for (int i = 0; i < 8192; ++i) {
		garbage.push_back(static_cast<char>((i * 131 + 7) & 0xff));
	}
	return garbage;
}

bool AnyQuarantined(const std::string &path)
{
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(fs::u8path(path).parent_path(), ec)) {
		if (entry.path().filename().u8string().rfind("chat.db.corrupt-", 0) == 0) {
			return true;
		}
	}
	return false;
}

// The schema version a read-only connection sees, or nothing when it cannot read one.
std::optional<int> ReadVersion(const std::string &path)
{
	sqlite3 *db = nullptr;
	std::optional<int> version;
	if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr) == SQLITE_OK &&
		    sqlite3_step(stmt) == SQLITE_ROW) {
			version = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	sqlite3_close_v2(db);
	return version;
}

// A private archive and the ring it backs, opened on `path`: one "launch".
Chat::ChatArchive::Options TestOptions(const std::string &path, Chat::Retention retention, const std::string &launchId,
				       std::vector<std::string> moderated = {"kick"})
{
	Chat::ChatArchive::Options options;
	options.path = path;
	options.retention = retention;
	options.launchId = launchId;
	options.moderatedPlatforms = std::move(moderated);
	return options;
}

struct Launch {
	Chat::ChatArchive archive;
	Chat::ChatHistory history{&archive};

	Launch(const std::string &path, Chat::Retention retention, const std::string &launchId,
	       std::vector<std::string> moderated = {"kick"})
	{
		history.OpenArchive(TestOptions(path, retention, launchId, std::move(moderated)));
	}
	~Launch() { archive.Shutdown(); }
	Launch(const Launch &) = delete;
	Launch &operator=(const Launch &) = delete;
};

// A second launch on `path` while a first ("L1", 7 days) holds it, so the second's store
// can open only once the first has gone.
struct Contended {
	Launch first;
	Chat::ChatArchive archive;
	Chat::ChatHistory history{&archive};

	explicit Contended(const std::string &path) : first(path, Chat::Retention::SevenDays, "L1") {}
	~Contended() { archive.Shutdown(); }
	Contended(const Contended &) = delete;
	Contended &operator=(const Contended &) = delete;

	// Boots the second launch; true when that leaves its store closed.
	bool Boot(const Chat::ChatArchive::Options &options)
	{
		first.archive.WaitIdle(kIdleWait);
		history.OpenArchive(options);
		const Chat::ArchiveStatus status = archive.Status();
		return status == Chat::ArchiveStatus::Disabled || status == Chat::ArchiveStatus::Off;
	}
	void Release()
	{
		first.archive.WaitIdle(kIdleWait);
		first.archive.Shutdown();
	}
	void Move(Chat::Retention to)
	{
		history.SetRetention(to);
		archive.WaitIdle(kIdleWait);
	}
};

// Each row's id and `deleted` mark, oldest first, as ReadOlder serves them.
std::string Marks(Chat::ChatArchive &archive)
{
	const std::optional<std::vector<json>> rows = archive.ReadOlder(UINT64_MAX, 100, Feed::Filter{});
	if (!rows) {
		return "unreadable";
	}
	std::string out;
	for (auto it = rows->rbegin(); it != rows->rend(); ++it) {
		out += (out.empty() ? "" : ",") + it->value("id", "") + "=" + it->value("deleted", "");
	}
	return out;
}

} // namespace

void ObsBootstrap::RunChatArchiveSelfTest()
{
	const fs::path dir =
		fs::temp_directory_path() / fs::u8path("braidcast-chat-archive-" + RandomUtil::HexToken(6));
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec) {
		Report("setup", false, "cannot create " + dir.u8string());
		return;
	}
	const auto dbPath = [&](const std::string &name) {
		fs::create_directories(dir / fs::u8path(name), ec);
		return (dir / fs::u8path(name) / "chat.db").u8string();
	};
	const auto ids = [](const std::string &prefix, int from, int to) {
		std::vector<std::string> out;
		for (int i = from; i < to; ++i) {
			out.push_back(prefix + std::to_string(i));
		}
		return out;
	};

	// Round trip: past the ring's cap the pages carry on from disk with no gap and no
	// repeat at the boundary, and the running count matches what was admitted.
	{
		const std::string path = dbPath("roundtrip");
		Launch launch(path, Chat::Retention::SevenDays, "L1");
		for (int i = 0; i < 1500; ++i) {
			Add(launch.history, kTwitch, "m" + std::to_string(i));
		}
		launch.archive.WaitIdle(kIdleWait);
		// The ring gives up its committed front on the next admission.
		Add(launch.history, kTwitch, "m1500");
		launch.archive.WaitIdle(kIdleWait);
		const Walk walk = WalkAll(launch.history, 37);
		const bool ok = walk.ordered && walk.ids == ids("m", 0, 1501) &&
				Scalar(path, "SELECT count(*) FROM messages") == 1501 &&
				launch.archive.Status() == Chat::ArchiveStatus::Ok;
		Report("round trip", ok, std::to_string(walk.ids.size()) + " of 1501 reached");

		// Redaction reaches a stored row before its write commits (the reader applies the
		// queued op) and after; a held row is redacted in place.
		launch.archive.HoldWrites(true);
		launch.history.Redact({kTwitch, Chat::ModerationAction::Delete, "m10", ""});
		launch.history.Redact({kTwitch, Chat::ModerationAction::ClearUser, "", "author-m1200"});
		const auto seqOf = [&](const std::string &id) {
			const auto at = std::find(walk.ids.begin(), walk.ids.end(), id);
			return at == walk.ids.end() ? 0 : walk.seqs[static_cast<size_t>(at - walk.ids.begin())];
		};
		const auto redacted = [&](const std::string &id) {
			const Chat::ChatPage page = launch.history.Page(seqOf(id) + 1, 1, Feed::Filter{});
			return page.items.size() == 1 && page.items[0].value("id", "") == id &&
			       page.items[0].value("deleted", "") != "" && page.items[0]["fragments"].empty();
		};
		const bool pendingOk = redacted("m10") && redacted("m1200") && !redacted("m11") &&
				       Scalar(path, "SELECT count(*) FROM messages WHERE deleted != ''") == 0;
		launch.archive.HoldWrites(false);
		launch.archive.WaitIdle(kIdleWait);
		const bool committedOk =
			redacted("m10") && redacted("m1200") &&
			Column(path, "SELECT msg_id FROM messages WHERE deleted != '' ORDER BY seq") == "m10,m1200" &&
			Scalar(path, "SELECT count(*) FROM messages WHERE msg_id = 'm10' AND body LIKE '%hello%'") == 0;
		Report("redaction", pendingOk && committedOk,
		       std::string("pending ") + (pendingOk ? "ok" : "bad") + ", committed " +
			       (committedOk ? "ok" : "bad"));

		// A Clear removes what was admitted before it and nothing after, in queue order, and
		// nothing it removed is served while its delete is still queued.
		launch.archive.HoldWrites(true);
		for (int i = 0; i < 5; ++i) {
			Add(launch.history, kTwitch, "c" + std::to_string(i));
		}
		launch.history.Clear();
		for (int i = 5; i < 8; ++i) {
			Add(launch.history, kTwitch, "c" + std::to_string(i));
		}
		const Walk whileQueued = WalkAll(launch.history);
		launch.archive.HoldWrites(false);
		launch.archive.WaitIdle(kIdleWait);
		const bool clearOk = whileQueued.ids == ids("c", 5, 8) &&
				     Column(path, "SELECT msg_id FROM messages ORDER BY seq") == "c5,c6,c7" &&
				     WalkAll(launch.history).ids == ids("c", 5, 8);
		Report("clear ordering", clearOk,
		       Joined(whileQueued.ids) + " | " + Column(path, "SELECT msg_id FROM messages ORDER BY seq"));

		// A retention sweep deleting rows under a reader paging continuously: every walk
		// stays strictly ordered, and nothing throws.
		for (int i = 0; i < 3000; ++i) {
			Add(launch.history, kTwitch, "s" + std::to_string(i));
		}
		launch.archive.WaitIdle(kIdleWait);
		std::atomic<bool> stop{false};
		std::atomic<int> walks{0};
		std::atomic<int> disordered{0};
		std::thread reader([&] {
			while (!stop.load()) {
				if (!WalkAll(launch.history, 41).ordered) {
					disordered.fetch_add(1);
				}
				walks.fetch_add(1);
			}
		});
		const int64_t aged = TimeUtil::NowMs() - 8 * TimeUtil::kDayMs;
		for (int chunk = 0; chunk < 6; ++chunk) {
			Scalar(path, "UPDATE messages SET rx = " + std::to_string(aged) +
					     " WHERE msg_id IN (SELECT msg_id FROM messages ORDER BY seq LIMIT 300)");
			launch.history.SetRetention(Chat::Retention::SevenDays); // sweeps
			launch.archive.WaitIdle(kIdleWait);
		}
		stop.store(true);
		reader.join();
		const int64_t left = Scalar(path, "SELECT count(*) FROM messages");
		const bool sweepOk = disordered.load() == 0 && walks.load() > 0 && left < 3003 && left > 0;
		Report("sweep under reader", sweepOk,
		       std::to_string(walks.load()) + " walks, " + std::to_string(disordered.load()) + " disordered, " +
			       std::to_string(left) + " rows left");
	}

	// A redaction reaches only rows below its seq bound, in the writer and in a reader's view
	// of it while it is still queued. b3 was committed before the op although its seq is past
	// the bound (the ring never orders them so, but neither path may rely on that), and b4
	// is queued after the op.
	{
		const std::string path = dbPath("redactbound");
		Chat::ChatArchive archive;
		Chat::ChatArchive::Seed seed;
		archive.Open(TestOptions(path, Chat::Retention::SevenDays, "L1"), seed);
		const auto enqueue = [&](uint64_t seq) {
			json frame = Frame(kTwitch, "b" + std::to_string(seq), "twitch");
			frame["author"]["id"] = "author-x";
			std::optional<Chat::ChatArchive::Row> row = Chat::ChatArchive::MakeRow(kTwitch, frame);
			if (row) {
				row->seq = seq;
				row->rx = TimeUtil::NowMs();
				archive.Enqueue(std::move(*row));
			}
		};
		enqueue(1);
		enqueue(2);
		enqueue(3);
		archive.WaitIdle(kIdleWait);
		archive.HoldWrites(true);
		archive.EnqueueRedact(
			{OAuth::DestinationKey(kTwitch), Chat::ModerationAction::ClearUser, "", "author-x", 3});
		enqueue(4);
		const std::string pending = Marks(archive);
		archive.HoldWrites(false);
		archive.WaitIdle(kIdleWait);
		const std::string committed = Marks(archive);
		const std::string stored = Column(path, "SELECT msg_id || '=' || deleted FROM messages ORDER BY seq");
		archive.Shutdown();
		const bool ok = pending == "b1=user,b2=user,b3=" && committed == "b1=user,b2=user,b3=,b4=" &&
				stored == committed;
		Report("redaction bound", ok, pending + " | " + committed + " | " + stored);
	}

	// A redaction with a platform time reaches only rows said at or before it, in the writer
	// and in a reader's view of it while it is queued: t1 is before the time, t2 exactly at
	// it, t3 after it, and t4 has no time at all, which a time-bounded op never reaches.
	{
		const std::string path = dbPath("redacttime");
		Chat::ChatArchive archive;
		Chat::ChatArchive::Seed seed;
		archive.Open(TestOptions(path, Chat::Retention::SevenDays, "L1"), seed);
		uint64_t seq = 0;
		const auto enqueue = [&](const std::string &id, int64_t ts) {
			json frame = Frame(kTwitch, id, "twitch");
			frame["author"]["id"] = "author-x";
			frame["ts"] = ts;
			std::optional<Chat::ChatArchive::Row> row = Chat::ChatArchive::MakeRow(kTwitch, frame);
			if (row) {
				row->seq = ++seq;
				row->rx = TimeUtil::NowMs();
				archive.Enqueue(std::move(*row));
			}
		};
		enqueue("t1", 100);
		enqueue("t2", 150);
		enqueue("t3", 151);
		enqueue("t4", 0);
		archive.WaitIdle(kIdleWait);
		archive.HoldWrites(true);
		Chat::Redaction timed{OAuth::DestinationKey(kTwitch), Chat::ModerationAction::ClearUser, "", "author-x",
				      seq + 1};
		timed.beforeTs = 150;
		archive.EnqueueRedact(timed);
		const std::string pending = Marks(archive);
		archive.HoldWrites(false);
		archive.WaitIdle(kIdleWait);
		const std::string committed = Marks(archive);
		const std::string stored = Column(path, "SELECT msg_id || '=' || deleted FROM messages ORDER BY seq");
		archive.Shutdown();
		const std::string want = "t1=user,t2=user,t3=,t4=";
		const bool ok = pending == want && committed == want && stored == want;
		Report("redaction time bound", ok, pending + " | " + committed + " | " + stored);
	}

	// Across a relaunch: rows of a platform whose moderation is not honored are gone, a
	// moderated platform's survive and seed the ring, and a re-delivery of a seeded message
	// is recognized. This session only: everything goes at the next launch.
	{
		const std::string path = dbPath("relaunch");
		{
			Launch first(path, Chat::Retention::SevenDays, "L1");
			for (int i = 0; i < 3; ++i) {
				Add(first.history, kTwitch, "t" + std::to_string(i));
				Add(first.history, kKickA, "k" + std::to_string(i), "kick");
			}
		}
		{
			Launch second(path, Chat::Retention::SevenDays, "L2");
			const Walk seeded = WalkAll(second.history);
			const bool redelivered = !Add(second.history, kKickA, "k1", "kick");
			// Seqs carry on past the newest kept row, so a new row never lands on one.
			json next = Frame(kTwitch, "t-next", "twitch");
			second.history.Add(kTwitch, next);
			const bool seqOk = !seeded.seqs.empty() && next.value("seq", uint64_t(0)) > seeded.seqs.back();
			Report("relaunch 7 days",
			       seeded.ordered && seeded.ids == ids("k", 0, 3) && redelivered && seqOk,
			       Joined(seeded.ids));
		}
		{
			Launch third(path, Chat::Retention::Session, "L3");
			const bool emptied = WalkAll(third.history).ids.empty();
			Add(third.history, kKickA, "session-0", "kick");
			third.archive.WaitIdle(kIdleWait);
			const bool written = Scalar(path, "SELECT count(*) FROM messages") == 1;
			third.archive.Shutdown();
			Launch fourth(path, Chat::Retention::Session, "L4");
			const bool purged = WalkAll(fourth.history).ids.empty() &&
					    Scalar(path, "SELECT count(*) FROM messages") == 0;
			Report("relaunch this session", emptied && written && purged);
		}
	}

	// A file that is not a database is set aside byte for byte and a fresh one started.
	{
		const std::string path = dbPath("corrupt");
		const std::string garbage = Garbage();
		{
			std::ofstream out(fs::u8path(path), std::ios::binary);
			out << garbage;
		}
		Launch launch(path, Chat::Retention::SevenDays, "L1");
		const std::string kept = launch.archive.StatusDetail();
		std::string quarantined;
		const bool readBack =
			!kept.empty() &&
			FileUtil::ReadUtf8File((fs::u8path(path).parent_path() / fs::u8path(kept)).u8string(),
					       quarantined);
		Add(launch.history, kTwitch, "fresh");
		launch.archive.WaitIdle(kIdleWait);
		const bool ok = launch.archive.Status() == Chat::ArchiveStatus::Recovered && readBack &&
				quarantined == garbage && Scalar(path, "SELECT count(*) FROM messages") == 1;
		Report("corrupt file quarantined", ok, kept);
	}

	// A newer build's file is left exactly as it is, persistence on or off, and the ring
	// still works.
	{
		const std::string path = dbPath("newer");
		WriteWalDb(path, kNewerVersion);
		const std::string before = Bytes(path);
		bool ringOk = false;
		Chat::ArchiveStatus status = Chat::ArchiveStatus::Ok;
		{
			Launch launch(path, Chat::Retention::SevenDays, "L1");
			status = launch.archive.Status();
			ringOk = Add(launch.history, kTwitch, "ring-only") && !launch.archive.Active() &&
				 WalkAll(launch.history).ids == std::vector<std::string>{"ring-only"};
		}
		{
			Launch off(path, Chat::Retention::Off, "L2");
		}
		const bool ok = status == Chat::ArchiveStatus::NewerSchema && ringOk && !before.empty() &&
				before == Bytes(path);
		Report("newer schema untouched", ok, std::to_string(before.size()) + " bytes");
	}

	// Shutdown commits a full queue: every admitted row reaches disk.
	{
		const std::string path = dbPath("shutdown");
		Launch launch(path, Chat::Retention::SevenDays, "L1");
		launch.archive.HoldWrites(true);
		for (int i = 0; i < 2500; ++i) {
			Add(launch.history, kTwitch, "q" + std::to_string(i));
		}
		launch.archive.Shutdown();
		const int64_t stored = Scalar(path, "SELECT count(*) FROM messages");
		Report("shutdown drains", stored == 2500, std::to_string(stored) + " of 2500");
	}

	// One writer per chat.db: a second archive on the same file stands down, and the lock
	// goes with the first archive's Shutdown.
	{
		const std::string path = dbPath("lock");
		bool ok = false;
		{
			Launch first(path, Chat::Retention::SevenDays, "L1");
			Launch second(path, Chat::Retention::SevenDays, "L2");
			ok = first.archive.Status() == Chat::ArchiveStatus::Ok &&
			     second.archive.Status() == Chat::ArchiveStatus::Disabled && !second.archive.Active() &&
			     Exists(path + ".lock");
		}
		Report("single writer", ok && !Exists(path + ".lock"));
	}

	// Off creates nothing; turning persistence on mid-session settles everything the ring
	// already holds, so the ring keeps to its cap before the first commit; Off again
	// removes the file.
	{
		const std::string path = dbPath("activate");
		Launch launch(path, Chat::Retention::Off, "L1");
		const bool nothingCreated = !Exists(path) && !Exists(path + ".lock");
		for (int i = 0; i < 1200; ++i) {
			Add(launch.history, kTwitch, "o" + std::to_string(i));
		}
		launch.archive.HoldWrites(true);
		launch.history.SetRetention(Chat::Retention::SevenDays);
		for (int i = 0; i < 50; ++i) {
			Add(launch.history, kTwitch, "a" + std::to_string(i));
		}
		// Nothing is committed yet, so every page comes from the ring, and chat.db is not
		// open to read: past the ring the walk learns "not known yet", never "the start".
		const Walk whileOpening = WalkAll(launch.history, 200);
		const size_t ringHeld = whileOpening.ids.size();
		launch.archive.HoldWrites(false);
		launch.archive.WaitIdle(kIdleWait);
		const bool readable = !WalkAll(launch.history, 200).unreadable;
		const bool stored = Scalar(path, "SELECT count(*) FROM messages") == 50;
		launch.history.SetRetention(Chat::Retention::Off);
		launch.archive.WaitIdle(kIdleWait);
		const bool removed = !Exists(path) && !Exists(path + ".lock");
		Report("activation",
		       nothingCreated && ringHeld == Chat::ChatHistory::kCap && whileOpening.unreadable && readable &&
			       stored && removed,
		       "ring " + std::to_string(ringHeld));
	}

	// Quarantined copies fall under retention: an expired one goes at open, a recent one
	// on Clear. A purge drops an account's rows, already hidden while it is queued.
	{
		const std::string path = dbPath("retention");
		const fs::path folder = fs::u8path(path).parent_path();
		const auto touch = [&](const std::string &name) {
			std::ofstream(folder / fs::u8path(name), std::ios::binary) << "x";
		};
		const std::string expired = "chat.db.corrupt-2000-01-01_00-00-00";
		const std::string recent = "chat.db.corrupt-" + TimeUtil::LocalFileStamp();
		touch(expired);
		touch(expired + "-wal");
		touch(recent);
		Launch launch(path, Chat::Retention::SevenDays, "L1");
		const bool expiredGone = !Exists((folder / fs::u8path(expired)).u8string()) &&
					 !Exists((folder / fs::u8path(expired + "-wal")).u8string()) &&
					 Exists((folder / fs::u8path(recent)).u8string());
		for (int i = 0; i < 4; ++i) {
			Add(launch.history, i % 2 ? kKickA : kKickB, "p" + std::to_string(i), "kick");
		}
		launch.archive.WaitIdle(kIdleWait);
		launch.archive.HoldWrites(true);
		launch.archive.PurgeAccount(kKickA.accountId);
		const std::vector<json> pending =
			launch.archive.ReadOlder(UINT64_MAX >> 1, 100, Feed::Filter{}).value_or(std::vector<json>{});
		const bool hidden = pending.size() == 2 &&
				    std::all_of(pending.begin(), pending.end(), [](const json &m) {
					    return m.value("accountId", "") == kKickB.accountId;
				    });
		launch.archive.HoldWrites(false);
		launch.archive.WaitIdle(kIdleWait);
		const bool purged = Scalar(path, "SELECT count(*) FROM messages WHERE account_id = 'kick:a'") == 0 &&
				    Scalar(path, "SELECT count(*) FROM messages") == 2;
		const bool goneOnPurge = !Exists((folder / fs::u8path(recent)).u8string());
		touch(recent);
		launch.history.Clear();
		launch.archive.WaitIdle(kIdleWait);
		const bool goneOnClear = !Exists((folder / fs::u8path(recent)).u8string());
		Report("quarantine retention", expiredGone && goneOnPurge && goneOnClear);
		Report("account purge", hidden && purged);
	}

	// A frame that cannot be serialized is refused as a row rather than failing a batch;
	// the ring repairs it first, so it is stored repaired.
	{
		json bad = Frame(kTwitch, "bad", "twitch");
		bad["fragments"][0]["text"] = std::string("caf\xC3(");
		const bool refused = !Chat::ChatArchive::MakeRow(kTwitch, bad).has_value();
		const std::string path = dbPath("badrow");
		Launch launch(path, Chat::Retention::SevenDays, "L1");
		const bool admitted = launch.history.Add(kTwitch, bad);
		Add(launch.history, kTwitch, "good");
		launch.archive.WaitIdle(kIdleWait);
		const bool ok = refused && admitted && Scalar(path, "SELECT count(*) FROM messages") == 2 &&
				launch.archive.Status() == Chat::ArchiveStatus::Ok;
		Report("bad row", ok);
	}

	// Off at open removes what an earlier launch stored, quarantined copies included.
	{
		const std::string path = dbPath("off");
		{
			Launch on(path, Chat::Retention::SevenDays, "L1");
			Add(on.history, kKickA, "x", "kick");
		}
		std::ofstream(fs::u8path(path).parent_path() / "chat.db.corrupt-2000-01-01_00-00-00", std::ios::binary)
			<< "x";
		const bool hadFile = Exists(path);
		{
			Launch off(path, Chat::Retention::Off, "L2");
		}
		bool anyLeft = false;
		for (const auto &entry : fs::directory_iterator(fs::u8path(path).parent_path(), ec)) {
			anyLeft = anyLeft || entry.path().filename().u8string().rfind("chat.db", 0) == 0;
		}
		Report("off removes", hadFile && !anyLeft);
	}

	// Degraded holds for the launch: turning chat history off and on again neither reports
	// Ok nor stores anything.
	{
		const std::string path = dbPath("degraded");
		Launch launch(path, Chat::Retention::SevenDays, "L1");
		Add(launch.history, kTwitch, "before");
		launch.archive.WaitIdle(kIdleWait);
		(void)launch.archive.Degrade("self-test");
		launch.history.SetRetention(Chat::Retention::Off);
		launch.archive.WaitIdle(kIdleWait);
		launch.history.SetRetention(Chat::Retention::SevenDays);
		launch.archive.WaitIdle(kIdleWait);
		Add(launch.history, kTwitch, "after");
		launch.archive.WaitIdle(kIdleWait);
		const bool ok = launch.archive.Status() == Chat::ArchiveStatus::Degraded && !launch.archive.Active() &&
				!Exists(path) &&
				WalkAll(launch.history).ids == std::vector<std::string>{"before", "after"};
		Report("degraded survives off and on", ok);
	}

	// Off since open starts no thread: a Clear, a redaction, a purge and Off again have
	// nothing on disk to reach. Turning persistence on is what starts one.
	{
		const std::string path = dbPath("offthread");
		Launch launch(path, Chat::Retention::Off, "L1");
		Add(launch.history, kTwitch, "o");
		launch.history.Clear();
		launch.history.Redact({kTwitch, Chat::ModerationAction::Delete, "o", ""});
		launch.archive.PurgeAccount(kTwitch.accountId);
		launch.history.SetRetention(Chat::Retention::Off);
		const bool idle = !launch.archive.WriterStarted() && !Exists(path) && !Exists(path + ".lock");
		launch.history.SetRetention(Chat::Retention::SevenDays);
		const bool started = launch.archive.WriterStarted();
		Report("off starts no thread", idle && started);
	}

	// Off never deletes a file whose version it could not read. A newer build's WAL file
	// whose -shm cannot be created (here, a directory stands in its way) fails the probe; it
	// is left as it is.
	{
		const std::string path = dbPath("probe");
		WriteWalDb(path, kNewerVersion);
		const std::string before = Bytes(path);
		fs::create_directory(fs::u8path(path + "-shm"), ec);
		const bool probeFails = !ReadVersion(path).has_value();
		{
			Launch off(path, Chat::Retention::Off, "L1");
		}
		const bool kept = Exists(path);
		fs::remove(fs::u8path(path + "-shm"), ec);
		const bool ok = probeFails && kept && Bytes(path) == before && ReadVersion(path) == kNewerVersion;
		Report("unreadable version kept", ok, probeFails ? "probe failed" : "probe read the file");
	}

	// Quarantined copies per mode: Session keeps none from an earlier launch; a newer
	// build's chat.db stays as it is while this build's copies still age out; an instance
	// that does not hold the lock touches nothing.
	{
		const auto touch = [](const fs::path &folder, const std::string &name) {
			std::ofstream(folder / fs::u8path(name), std::ios::binary) << "x";
		};
		const std::string expired = "chat.db.corrupt-2000-01-01_00-00-00";
		const std::string recent = "chat.db.corrupt-" + TimeUtil::LocalFileStamp();
		const auto has = [](const fs::path &folder, const std::string &name) {
			return Exists((folder / fs::u8path(name)).u8string());
		};

		const std::string sessionPath = dbPath("qsession");
		const fs::path sessionDir = fs::u8path(sessionPath).parent_path();
		touch(sessionDir, recent);
		bool sessionOk = false;
		{
			Launch launch(sessionPath, Chat::Retention::Session, "L1");
			sessionOk = !has(sessionDir, recent) && launch.archive.Status() == Chat::ArchiveStatus::Ok;
		}

		const std::string newerPath = dbPath("qnewer");
		const fs::path newerDir = fs::u8path(newerPath).parent_path();
		WriteWalDb(newerPath, kNewerVersion);
		const std::string newerBytes = Bytes(newerPath);
		touch(newerDir, expired);
		touch(newerDir, recent);
		bool newerOk = false;
		{
			Launch launch(newerPath, Chat::Retention::SevenDays, "L1");
			newerOk = launch.archive.Status() == Chat::ArchiveStatus::NewerSchema &&
				  !has(newerDir, expired) && has(newerDir, recent);
		}
		newerOk = newerOk && Bytes(newerPath) == newerBytes;

		const std::string lockedPath = dbPath("qlocked");
		const fs::path lockedDir = fs::u8path(lockedPath).parent_path();
		bool lockedOk = false;
		{
			Launch owner(lockedPath, Chat::Retention::SevenDays, "L1");
			touch(lockedDir, expired);
			Launch second(lockedPath, Chat::Retention::Session, "L2");
			lockedOk = second.archive.Status() == Chat::ArchiveStatus::Disabled && has(lockedDir, expired);
		}
		Report("quarantine per mode", sessionOk && newerOk && lockedOk,
		       std::string("session ") + (sessionOk ? "ok" : "bad") + ", newer " + (newerOk ? "ok" : "bad") +
			       ", locked " + (lockedOk ? "ok" : "bad"));
	}

	// In Session mode a corrupt file holds only earlier launches' chat, so it is deleted
	// rather than set aside, and nothing names a copy that is not there.
	{
		const std::string path = dbPath("corruptsession");
		{
			std::ofstream out(fs::u8path(path), std::ios::binary);
			out << Garbage();
		}
		Launch launch(path, Chat::Retention::Session, "L1");
		const std::string detail = launch.archive.StatusDetail();
		Add(launch.history, kTwitch, "fresh");
		launch.archive.WaitIdle(kIdleWait);
		const bool ok = launch.archive.Status() == Chat::ArchiveStatus::Recovered && !AnyQuarantined(path) &&
				detail.empty() && Scalar(path, "SELECT count(*) FROM messages") == 1;
		Report("corrupt file this session", ok, detail);
	}

	// A writer thread that cannot be started takes the store it opened with it, and the
	// quarantined copies, since a purge, Clear or Off may never reach them. The ring keeps
	// what was seeded, and the control ops that follow neither throw nor reach disk.
	{
		const std::string path = dbPath("spawnfail");
		{
			Launch first(path, Chat::Retention::SevenDays, "L1");
			Add(first.history, kKickA, "k0", "kick");
		}
		{
			// Stamped now, so that retention does not age it out first.
			std::ofstream out(fs::u8path(path + ".corrupt-" + TimeUtil::LocalFileStamp()),
					  std::ios::binary);
			out << Garbage();
		}
		Chat::ChatArchive archive;
		Chat::ChatHistory history{&archive};
		archive.FailWriterStart(true);
		history.OpenArchive(TestOptions(path, Chat::Retention::SevenDays, "L2"));
		const bool degraded = archive.Status() == Chat::ArchiveStatus::Degraded && !archive.Active();
		const bool removed = !Exists(path) && !Exists(path + "-wal") && !Exists(path + ".lock") &&
				     !AnyQuarantined(path);
		const bool seeded = WalkAll(history).ids == std::vector<std::string>{"k0"};
		archive.PurgeAccount(kKickA.accountId);
		history.Clear();
		history.SetRetention(Chat::Retention::Off);
		const bool quiet = !archive.WriterStarted() && !Exists(path);
		archive.Shutdown();
		Report("spawn failure removes the store", degraded && removed && seeded && quiet);
	}

	// A store the failed writer could not delete is owed its removal: the next control op
	// that does start a writer makes it, although chat history is still on.
	{
		const std::string path = dbPath("spawnretry");
		{
			Launch first(path, Chat::Retention::SevenDays, "L1");
			Add(first.history, kKickA, "k0", "kick");
		}
		Chat::ChatArchive archive;
		Chat::ChatHistory history{&archive};
		archive.FailWriterStart(true);
		bool leftAtOpen = false;
		{
			// Held open without delete sharing, chat.db cannot be deleted.
			std::ifstream hold(fs::u8path(path), std::ios::binary);
			history.OpenArchive(TestOptions(path, Chat::Retention::SevenDays, "L2"));
			leftAtOpen = hold.is_open() && Exists(path) &&
				     archive.Status() == Chat::ArchiveStatus::Degraded;
		}
		archive.FailWriterStart(false);
		history.Clear();
		archive.WaitIdle(kIdleWait);
		const bool removed = archive.WriterStarted() && !Exists(path) && !Exists(path + "-wal");
		archive.Shutdown();
		Report("spawn failure retries a failed delete", leftAtOpen && removed);
	}

	// Off that could not read a file's version owes it another try: once the file can be
	// read, the next control op removes it. (Off since open otherwise starts no thread.)
	{
		const std::string path = dbPath("owed");
		WriteWalDb(path, Chat::kChatLadder.Current());
		fs::create_directory(fs::u8path(path + "-shm"), ec);
		Launch launch(path, Chat::Retention::Off, "L1");
		const bool leftAtOpen = Exists(path) && !launch.archive.WriterStarted();
		fs::remove(fs::u8path(path + "-shm"), ec);
		launch.history.Clear();
		launch.archive.WaitIdle(kIdleWait);
		const bool removed = !Exists(path) && launch.archive.WriterStarted();
		Report("off retries an unreadable file", leftAtOpen && removed);
	}

	// A chat.db left because its version could not be read is reported by name, with the file
	// on disk, for as long as its removal is owed: through a Clear that still cannot read it,
	// until one that can removes it (the status is plain off again). A file that becomes the
	// live store (7 days chosen once it can be read) is no longer reported either.
	{
		const auto seedStore = [&](const std::string &path) {
			{
				Launch first(path, Chat::Retention::SevenDays, "L1");
				Add(first.history, kKickA, "k0", "kick");
			}
			fs::create_directory(fs::u8path(path + "-shm"), ec);
		};
		const auto reported = [](const json &status, const std::string &name) {
			return status.value("status", "") == name;
		};
		// Checked as each report is taken, since the file is gone by the end.
		const auto kept = [&](const json &status, const std::string &path) {
			return reported(status, "unreadable") && status.value("detail", "") == "chat.db" &&
			       status.value("onDisk", false) && Exists(path);
		};

		const std::string path = dbPath("unreadable");
		seedStore(path);
		Launch launch(path, Chat::Retention::Off, "L2");
		const json atOpen = launch.archive.StatusJson(kIdleWait);
		const bool keptAtOpen = kept(atOpen, path);
		launch.history.Clear();
		launch.archive.WaitIdle(kIdleWait);
		const json afterClear = launch.archive.StatusJson(kIdleWait);
		const bool keptAfterClear = kept(afterClear, path);
		fs::remove(fs::u8path(path + "-shm"), ec);
		launch.history.Clear();
		launch.archive.WaitIdle(kIdleWait);
		const json removed = launch.archive.StatusJson(kIdleWait);
		const bool removedOk = keptAtOpen && keptAfterClear && reported(removed, "off") &&
				       removed.value("detail", "x").empty() && !removed.value("onDisk", true) &&
				       !Exists(path);

		const std::string livePath = dbPath("unreadable-live");
		seedStore(livePath);
		Launch live(livePath, Chat::Retention::Off, "L2");
		const json liveAtOpen = live.archive.StatusJson(kIdleWait);
		const bool liveKeptAtOpen = kept(liveAtOpen, livePath);
		fs::remove(fs::u8path(livePath + "-shm"), ec);
		live.history.SetRetention(Chat::Retention::SevenDays);
		live.archive.WaitIdle(kIdleWait);
		const json adopted = live.archive.StatusJson(kIdleWait);
		const bool liveOk = liveKeptAtOpen && reported(adopted, "ok") && adopted.value("detail", "x").empty() &&
				    adopted.value("onDisk", false) && Exists(livePath);

		Report("unreadable file reported", removedOk && liveOk,
		       atOpen.dump() + " | " + afterClear.dump() + " | " + removed.dump() + " | " + liveAtOpen.dump() +
			       " | " + adopted.dump());

		// A retry that finds the file held by another instance probes nothing, so the file
		// stays reported, and the owe stays: once that instance exits, the next Clear removes it.
		const std::string heldPath = dbPath("unreadable-held");
		seedStore(heldPath);
		Launch held(heldPath, Chat::Retention::Off, "L2");
		const bool heldAtOpen = kept(held.archive.StatusJson(kIdleWait), heldPath);
		fs::remove(fs::u8path(heldPath + "-shm"), ec);
		json whileHeld;
		bool keptWhileHeld = false;
		{
			Launch other(heldPath, Chat::Retention::SevenDays, "L3");
			held.history.Clear();
			held.archive.WaitIdle(kIdleWait);
			whileHeld = held.archive.StatusJson(kIdleWait);
			keptWhileHeld = kept(whileHeld, heldPath) && other.archive.Status() == Chat::ArchiveStatus::Ok;
		}
		held.history.Clear();
		held.archive.WaitIdle(kIdleWait);
		const json released = held.archive.StatusJson(kIdleWait);
		const bool heldOk = heldAtOpen && keptWhileHeld && reported(released, "off") && !Exists(heldPath);
		Report("unreadable file survives a held retry", heldOk, whileHeld.dump() + " | " + released.dump());

		// Degraded holds for the launch, so it stays the status, and its detail names the file.
		const std::string degradedPath = dbPath("unreadable-degraded");
		seedStore(degradedPath);
		Launch degraded(degradedPath, Chat::Retention::Off, "L2");
		(void)degraded.archive.Degrade("self-test");
		const json named = degraded.archive.StatusJson(kIdleWait);
		const bool degradedOk = reported(named, "degraded") &&
					named.value("detail", "") ==
						"self-test; chat.db could not be read and was left in place" &&
					named.value("onDisk", false);
		Report("degraded names an unreadable file", degradedOk, named.dump());

		// A kept file deleted from outside is no longer named, though its removal is still owed.
		const std::string gonePath = dbPath("unreadable-gone");
		seedStore(gonePath);
		Launch gone(gonePath, Chat::Retention::Off, "L2");
		const bool goneAtOpen = kept(gone.archive.StatusJson(kIdleWait), gonePath);
		fs::remove(fs::u8path(gonePath + "-shm"), ec);
		fs::remove(fs::u8path(gonePath), ec);
		const json afterDelete = gone.archive.StatusJson(kIdleWait);
		const bool goneOk = goneAtOpen && reported(afterDelete, "off") &&
				    afterDelete.value("detail", "x").empty() && !afterDelete.value("onDisk", true);
		Report("unreadable file deleted outside", goneOk, afterDelete.dump());
	}

	// The report chat.historyStatus answers with: the count a reader can reach (a queued
	// Clear already hides its rows), the status and detail, whether a file is on disk, and
	// the moderated platforms. It waits for a control op queued before it, not for rows.
	{
		const std::string path = dbPath("report");
		Launch launch(path, Chat::Retention::SevenDays, "L1");
		for (int i = 0; i < 3; ++i) {
			Add(launch.history, kTwitch, "r" + std::to_string(i));
		}
		launch.archive.WaitIdle(kIdleWait);
		const json stored = launch.archive.StatusJson(kIdleWait);
		Add(launch.history, kTwitch, "r3"); // a row in its batching delay
		const auto askedAt = std::chrono::steady_clock::now();
		launch.archive.StatusJson(kIdleWait);
		const auto waited = std::chrono::steady_clock::now() - askedAt;
		launch.archive.HoldWrites(true);
		launch.history.Clear();
		const json cleared = launch.archive.StatusJson(0ms);
		launch.archive.HoldWrites(false);
		launch.history.SetRetention(Chat::Retention::Off);
		const json off = launch.archive.StatusJson(kIdleWait);

		const std::string newerPath = dbPath("report-newer");
		WriteWalDb(newerPath, kNewerVersion);
		Launch newer(newerPath, Chat::Retention::SevenDays, "L1");
		const json refused = newer.archive.StatusJson(kIdleWait);

		const bool ok = stored == json{{"status", "ok"},
					       {"detail", ""},
					       {"rows", 3},
					       {"onDisk", true},
					       {"moderatedPlatforms", json::array({"kick"})}} &&
				waited < 200ms && cleared.value("rows", -1) == 0 && off.value("status", "") == "off" &&
				off.value("rows", -1) == 0 && !off.value("onDisk", true) &&
				refused.value("status", "") == "newer-schema" && !refused.value("detail", "").empty() &&
				refused.value("onDisk", false);
		Report("status report", ok,
		       stored.dump() + " | " +
			       std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()) +
			       " ms | " + cleared.dump() + " | " + off.dump() + " | " + refused.dump());
	}

	// A stored retention this build does not know runs as 7 days that writes no new row.
	// Open sweeps as 7 days does (the age limit, earlier launches' rows on a platform whose
	// moderation is not honored, expired copies) and seeds this launch's seqs above what is
	// stored; purges, redactions and Clear still reach the store. Moving to 7 days keeps
	// every row, and Off removes the store.
	{
		const auto held = [&](const std::string &path, const std::string &launchId) {
			auto options = TestOptions(path, Chat::Retention::SevenDays, launchId);
			options.unknownSetting = "7D";
			return options;
		};
		const auto count = [](const std::string &path, const std::string &where = "1") {
			return Scalar(path, "SELECT count(*) FROM messages WHERE " + where);
		};
		const auto seed = [&](const std::string &path) {
			Launch first(path, Chat::Retention::SevenDays, "L1");
			Add(first.history, kKickA, "a0", "kick");
			Add(first.history, kKickA, "a1", "kick");
			Add(first.history, kKickB, "b0", "kick");
			Add(first.history, kKickB, "old", "kick");
			Add(first.history, kTwitch, "t0", "twitch");
			first.archive.WaitIdle(kIdleWait);
		};

		const std::string path = dbPath("unknown");
		seed(path);
		Scalar(path, "UPDATE messages SET rx = " + std::to_string(TimeUtil::NowMs() - 8 * TimeUtil::kDayMs) +
				     " WHERE msg_id = 'old'");
		const fs::path expired = fs::u8path(path).parent_path() / "chat.db.corrupt-2000-01-01_00-00-00";
		std::ofstream(expired, std::ios::binary) << "x";
		const std::string a1Before = Column(path, "SELECT body FROM messages WHERE msg_id = 'a1'");
		bool opened = false, redacted = false, purged = false, paged = false, cleared = false;
		std::string report;
		{
			Chat::ChatArchive archive;
			Chat::ChatHistory history{&archive};
			history.OpenArchive(held(path, "L2"));
			const json status = archive.StatusJson(kIdleWait);
			opened = status.value("status", "") == "unknown-setting" &&
				 status.value("detail", "") == "7D" && archive.WriterStarted() && !archive.Active() &&
				 count(path) == 3 && count(path, "msg_id IN ('old', 't0')") == 0 &&
				 !Exists(expired.u8string());

			Add(history, kKickA, "n1", "kick");
			history.Redact({kKickA, Chat::ModerationAction::Delete, "a1", ""});
			archive.WaitIdle(kIdleWait);
			const std::string a1After = Column(path, "SELECT body FROM messages WHERE msg_id = 'a1'");
			redacted = !a1After.empty() && a1After != a1Before && count(path, "msg_id = 'n1'") == 0;

			archive.PurgeAccount(kKickA.accountId);
			archive.WaitIdle(kIdleWait);
			purged = Column(path, "SELECT msg_id FROM messages") == "b0";

			// The ring evicts the seeded rows and n1 without waiting on the disk; paging past
			// it reaches the stored b0, and n1, which was never written, is gone.
			for (const std::string &id : ids("f", 0, 1000)) {
				Add(history, kKickB, id, "kick");
			}
			const Walk walk = WalkAll(history, 200);
			paged = walk.ordered && !walk.unreadable && walk.ids.size() == 1001 &&
				walk.ids.front() == "b0" && count(path) == 1;

			history.Clear();
			const json afterClear = archive.StatusJson(kIdleWait);
			cleared = Exists(path) && count(path) == 0 &&
				  afterClear.value("status", "") == "unknown-setting" &&
				  afterClear.value("rows", -1) == 0;
			report = status.dump() + " | " + std::to_string(walk.ids.size()) + " paged | " +
				 afterClear.dump();
			archive.Shutdown();
		}

		const std::string keepPath = dbPath("unknown-keep");
		seed(keepPath);
		const int64_t storedMax = Scalar(keepPath, "SELECT max(seq) FROM messages");
		bool keeps = false;
		{
			Chat::ChatArchive archive;
			Chat::ChatHistory history{&archive};
			history.OpenArchive(held(keepPath, "L2"));
			const int64_t heldRows = count(keepPath); // the twitch row is gone, as under 7 days
			Add(history, kKickA, "k1", "kick");
			history.SetRetention(Chat::Retention::SevenDays);
			Add(history, kKickA, "k2", "kick");
			archive.WaitIdle(kIdleWait);
			keeps = archive.Status() == Chat::ArchiveStatus::Ok && archive.StatusDetail().empty() &&
				count(keepPath) == heldRows + 1 && count(keepPath, "msg_id = 'k1'") == 0 &&
				Scalar(keepPath, "SELECT seq FROM messages WHERE msg_id = 'k2'") > storedMax &&
				WalkAll(history).ordered;
			archive.Shutdown();
		}

		const std::string offPath = dbPath("unknown-off");
		seed(offPath);
		bool offRemoves = false;
		{
			Chat::ChatArchive archive;
			Chat::ChatHistory history{&archive};
			history.OpenArchive(held(offPath, "L2"));
			const bool kept = Exists(offPath);
			history.SetRetention(Chat::Retention::Off);
			archive.WaitIdle(kIdleWait);
			offRemoves = kept && !Exists(offPath) && archive.Status() == Chat::ArchiveStatus::Off;
			archive.Shutdown();
		}
		const bool ok = opened && redacted && purged && paged && cleared && keeps && offRemoves;
		Report("unknown setting holds", ok,
		       report + " | opened " + std::to_string(opened) + " redacted " + std::to_string(redacted) +
			       " purged " + std::to_string(purged) + " paged " + std::to_string(paged) + " cleared " +
			       std::to_string(cleared) + " keeps " + std::to_string(keeps) + " off " +
			       std::to_string(offRemoves));
	}

	// A store that opens after boot keeps what another instance stored there. The first
	// launch holds chat.db, so the second boots with it Disabled, admits a few messages, and
	// removes an account; once the first has exited it moves to 7 days. The first launch's
	// rows stay, bar the removed account's; a row admitted before the store opened whose seq
	// falls among the stored ones stays in the ring only; new seqs sit above the stored
	// maximum; and paging reaches both sets in seq order, each once, the stored rows once the
	// held ones below them have left the ring.
	{
		const std::string path = dbPath("late-open");
		const auto count = [&](const std::string &where) {
			return Scalar(path, "SELECT count(*) FROM messages WHERE " + where);
		};
		Contended c(path);
		for (const std::string &id : ids("a", 0, 4)) {
			Add(c.first.history, kKickA, id, "kick");
		}
		Add(c.first.history, kKickB, "gone", "kick");
		const bool disabled = c.Boot(TestOptions(path, Chat::Retention::Session, "L2"));
		const int64_t storedMax = Scalar(path, "SELECT max(seq) FROM messages");
		for (const std::string &id : ids("b", 0, 3)) {
			Add(c.history, kKickA, id, "kick");
		}
		c.archive.PurgeAccount(kKickB.accountId);
		c.archive.WaitIdle(kIdleWait);
		c.Release();

		// The writer is held, so the store opens only after w0 is admitted: w0's seq (4) is
		// one the first launch stored.
		c.archive.HoldWrites(true);
		c.history.SetRetention(Chat::Retention::SevenDays);
		Add(c.history, kKickA, "w0", "kick");
		c.archive.HoldWrites(false);
		c.archive.WaitIdle(kIdleWait);
		for (const std::string &id : ids("n", 0, 3)) {
			Add(c.history, kKickA, id, "kick");
		}
		c.archive.WaitIdle(kIdleWait);
		const bool kept = c.archive.Status() == Chat::ArchiveStatus::Ok && count("launch_id = 'L1'") == 4 &&
				  count("msg_id = 'gone'") == 0 && count("msg_id = 'w0'") == 0 &&
				  count("launch_id = 'L2'") == 3 &&
				  Scalar(path, "SELECT min(seq) FROM messages WHERE launch_id = 'L2'") > storedMax;
		// The ring still holds b0..w0 at seqs the store also uses, so the walk stops at them.
		const Walk held = WalkAll(c.history, 2);
		const std::vector<std::string> heldWant{"b0", "b1", "b2", "w0", "n0", "n1", "n2"};

		for (const std::string &id : ids("f", 0, 1000)) {
			Add(c.history, kKickA, id, "kick");
		}
		c.archive.WaitIdle(kIdleWait);
		Add(c.history, kKickA, "f1000", "kick"); // the ring gives up its settled front
		c.archive.WaitIdle(kIdleWait);
		const Walk turned = WalkAll(c.history, 37);
		std::vector<std::string> turnedWant = ids("a", 0, 4);
		for (const std::string &id : ids("n", 0, 3)) {
			turnedWant.push_back(id);
		}
		for (const std::string &id : ids("f", 0, 1001)) {
			turnedWant.push_back(id);
		}

		c.history.Clear();
		c.archive.WaitIdle(kIdleWait);
		const bool cleared = count("1") == 0;
		const bool ok = disabled && kept && held.ordered && !held.unreadable && held.ids == heldWant &&
				turned.ordered && !turned.unreadable && turned.ids == turnedWant && cleared;
		Report("late open keeps rows", ok,
		       "stored max " + std::to_string(storedMax) + ", held " + Joined(held.ids) + ", turned " +
			       std::to_string(turned.ids.size()) + " of " + std::to_string(turnedWant.size()) +
			       ", disabled " + std::to_string(disabled) + " kept " + std::to_string(kept) +
			       " cleared " + std::to_string(cleared));
	}

	// Which late opens delete. None of the moves the user is not asked to confirm (Off to
	// either, the unknown setting to 7 days) does. 7 days or the unknown setting to This
	// session asks, and removes what the other instance stored. So does a move the user
	// confirmed while the other instance held the store (to Off, or 7 days to This session,
	// whose open failed): the removal is owed, and a later move to 7 days makes it.
	{
		using R = Chat::Retention;
		const auto left = [&](const std::string &name, R boot, bool unknown, const std::vector<R> &whileHeld,
				      R to) {
			const std::string path = dbPath(name);
			Contended c(path);
			Add(c.first.history, kKickA, "x0", "kick");
			Add(c.first.history, kKickA, "x1", "kick");
			auto options = TestOptions(path, boot, "L2");
			if (unknown) {
				options.unknownSetting = "7D";
			}
			const bool closed = c.Boot(options);
			for (R move : whileHeld) {
				c.Move(move);
			}
			c.Release();
			c.Move(to);
			const int64_t rows = Scalar(path, "SELECT count(*) FROM messages WHERE launch_id = 'L1'");
			return closed ? rows : -1;
		};
		struct Case {
			std::string name;
			int64_t rows;
			int64_t want;
		};
		const std::vector<Case> cases{
			{"off>7d", left("late-off-7d", R::Off, false, {}, R::SevenDays), 2},
			{"off>session", left("late-off-session", R::Off, false, {}, R::Session), 2},
			{"unknown>7d", left("late-unknown-7d", R::SevenDays, true, {}, R::SevenDays), 2},
			{"7d>session", left("late-7d-session", R::SevenDays, false, {}, R::Session), 0},
			{"unknown>session", left("late-unknown-session", R::SevenDays, true, {}, R::Session), 0},
			{"7d>off>7d", left("late-owed-off", R::SevenDays, false, {R::Off}, R::SevenDays), 0},
			{"7d>session>7d", left("late-owed-session", R::SevenDays, false, {R::Session}, R::SevenDays),
			 0},
		};
		bool ok = true;
		std::string detail;
		for (const Case &each : cases) {
			ok = ok && each.rows == each.want;
			detail += (detail.empty() ? "" : ", ") + each.name + " " + std::to_string(each.rows);
		}
		Report("late open deletes only when asked", ok, detail);
	}

	// What a launch booted with Off removed while the other held the store reaches the store
	// once it opens: an account purge; a moderator's delete by id, which reaches the stored
	// copy whatever its seq; and an author-wide removal, bounded by when it was seen (x-late
	// was said after it). The removals kept are capped at the newest 4096, so the oldest, the
	// delete of a1, is dropped.
	{
		const std::string path = dbPath("late-missed");
		Contended c(path);
		AddFrom(c.first.history, kKickA, "a0", "author-a0", 1000);
		AddFrom(c.first.history, kKickA, "a1", "author-a1", 1000);
		AddFrom(c.first.history, kKickA, "x-early", "author-x", 1000);
		AddFrom(c.first.history, kKickA, "x-late", "author-x", TimeUtil::NowMs() + 10 * 60 * 1000);
		Add(c.first.history, kKickB, "p0", "kick");
		const bool closed = c.Boot(TestOptions(path, Chat::Retention::Off, "L2"));
		c.archive.PurgeAccount(kKickB.accountId);
		c.history.Redact({kKickA, Chat::ModerationAction::Delete, "a1", ""});
		for (int i = 0; i < 4094; ++i) {
			c.history.Redact({kKickA, Chat::ModerationAction::Delete, "none" + std::to_string(i), ""});
		}
		c.history.Redact({kKickA, Chat::ModerationAction::Delete, "a0", ""});
		c.history.Redact({kKickA, Chat::ModerationAction::ClearUser, "", "author-x"});
		const bool quiet = !c.archive.WriterStarted();
		c.Release();
		c.Move(Chat::Retention::SevenDays);
		const std::string marks = Marks(c.archive);
		const bool ok = closed && quiet && marks == "a0=message,a1=,x-early=user,x-late=";
		Report("late open replays what it missed", ok, marks + ", quiet " + std::to_string(quiet));
	}

	// A Clear made while the store was closed reaches the rows received before it, not the
	// one the other instance received after (c2); and a Clear made after the move but before
	// the store opened, whose bound (2) is below every stored seq, still reaches them all.
	{
		const std::string missedPath = dbPath("late-clear");
		Contended missed(missedPath);
		Add(missed.first.history, kKickA, "c0", "kick");
		Add(missed.first.history, kKickA, "c1", "kick");
		const bool missedClosed = missed.Boot(TestOptions(missedPath, Chat::Retention::Off, "L2"));
		std::this_thread::sleep_for(20ms);
		missed.history.Clear();
		std::this_thread::sleep_for(20ms);
		Add(missed.first.history, kKickA, "c2", "kick");
		missed.Release();
		missed.Move(Chat::Retention::SevenDays);
		const std::string afterMissed = Marks(missed.archive);

		const std::string windowPath = dbPath("late-clear-window");
		Contended window(windowPath);
		for (const std::string &id : ids("d", 0, 6)) {
			Add(window.first.history, kKickA, id, "kick");
		}
		const bool windowClosed = window.Boot(TestOptions(windowPath, Chat::Retention::Session, "L2"));
		Add(window.history, kKickA, "b0", "kick");
		window.Release();
		window.archive.HoldWrites(true);
		window.history.SetRetention(Chat::Retention::SevenDays);
		window.history.Clear();
		window.archive.HoldWrites(false);
		window.archive.WaitIdle(kIdleWait);
		const std::string afterWindow = Marks(window.archive);
		const int64_t windowRows = Scalar(windowPath, "SELECT count(*) FROM messages");
		const bool ok = missedClosed && afterMissed == "c2=" && windowClosed && afterWindow.empty() &&
				windowRows == 0;
		Report("late open replays a Clear", ok,
		       afterMissed + " | " + afterWindow + " | " + std::to_string(windowRows) + " rows");
	}

	// A mode changed twice around another instance's run: 7 days, then Off (which removes
	// this launch's store); the other instance stores x0 and x1 and exits; 7 days again keeps
	// them, this launch's seqs continuing above theirs; and a confirmed move to This session
	// then removes them, keeping this launch's.
	{
		const std::string path = dbPath("late-twice");
		Launch second(path, Chat::Retention::SevenDays, "L2");
		Add(second.history, kKickA, "own0", "kick");
		second.archive.WaitIdle(kIdleWait);
		second.history.SetRetention(Chat::Retention::Off);
		second.archive.WaitIdle(kIdleWait);
		const bool removed = !Exists(path);
		{
			Launch other(path, Chat::Retention::SevenDays, "L1");
			Add(other.history, kKickA, "x0", "kick");
			Add(other.history, kKickA, "x1", "kick");
		}
		const int64_t storedMax = Scalar(path, "SELECT max(seq) FROM messages");
		second.history.SetRetention(Chat::Retention::SevenDays);
		second.archive.WaitIdle(kIdleWait);
		Add(second.history, kKickA, "own1", "kick");
		second.archive.WaitIdle(kIdleWait);
		const std::string kept = Marks(second.archive);
		const bool above = Scalar(path, "SELECT seq FROM messages WHERE msg_id = 'own1'") > storedMax;
		second.history.SetRetention(Chat::Retention::Session);
		second.archive.WaitIdle(kIdleWait);
		const std::string session = Marks(second.archive);
		const bool ok = removed && kept == "x0=,x1=,own1=" && above && session == "own1=";
		Report("late open after a mode changed twice", ok,
		       kept + " | " + session + ", removed " + std::to_string(removed) + " above " +
			       std::to_string(above));
	}

	// Moving to This session from Off asks nothing, so what the other instance stored stays
	// for the launch, through a later sweep too, and goes at the next launch's open.
	{
		const std::string path = dbPath("late-session-keeps");
		std::string adopted;
		std::string swept;
		bool closed = false;
		{
			Contended c(path);
			Add(c.first.history, kKickA, "x0", "kick");
			closed = c.Boot(TestOptions(path, Chat::Retention::Off, "L2"));
			c.Release();
			c.Move(Chat::Retention::Session);
			adopted = Marks(c.archive);
			c.Move(Chat::Retention::Session); // sweeps again
			swept = Marks(c.archive);
		}
		Launch next(path, Chat::Retention::Session, "L3");
		const std::string nextLaunch = Marks(next.archive);
		const bool ok = closed && adopted == "x0=" && swept == "x0=" && nextLaunch.empty();
		Report("late open to this session keeps for the launch", ok,
		       adopted + " | " + swept + " | " + nextLaunch);
	}

	// Nothing is served from an adopted store before its replay and retention sweep have run:
	// a reader polling throughout never sees the purged account's row or an earlier launch's
	// row on a platform whose moderation is not honored. When either step fails nothing is
	// served at all: a failed replay removes the store, which no longer holds what the user
	// left; a failed sweep leaves it for the next launch.
	{
		const std::string path = dbPath("late-reader");
		Contended c(path);
		Add(c.first.history, kKickA, "keep", "kick");
		Add(c.first.history, kKickB, "purged", "kick");
		Add(c.first.history, kTwitch, "unmoderated", "twitch");
		const bool closed = c.Boot(TestOptions(path, Chat::Retention::Off, "L2"));
		c.archive.PurgeAccount(kKickB.accountId);
		c.Release();
		std::atomic<bool> stop{false};
		std::atomic<int> leaked{0};
		std::atomic<int> served{0};
		std::thread reader([&] {
			while (!stop.load()) {
				const std::string marks = Marks(c.archive);
				if (marks.find("purged") != std::string::npos ||
				    marks.find("unmoderated") != std::string::npos) {
					++leaked;
				}
				if (marks == "keep=") {
					++served;
				}
			}
		});
		c.Move(Chat::Retention::SevenDays);
		std::this_thread::sleep_for(20ms);
		stop.store(true);
		reader.join();
		const bool ordered = closed && leaked.load() == 0 && served.load() > 0;

		const auto failing = [&](const std::string &name, bool purge, bool &left) {
			const std::string failPath = dbPath(name);
			Contended f(failPath);
			Add(f.first.history, kKickA, "k0", "kick");
			Add(f.first.history, kKickB, "k1", "kick");
			const bool fClosed = f.Boot(TestOptions(failPath, Chat::Retention::Off, "L2"));
			if (purge) {
				f.archive.PurgeAccount(kKickB.accountId);
			}
			f.Release();
			f.archive.FailWrites(true);
			f.Move(Chat::Retention::SevenDays);
			left = Exists(failPath);
			return fClosed && f.archive.Status() == Chat::ArchiveStatus::Degraded &&
			       Marks(f.archive).empty() && WalkAll(f.history).ids.empty();
		};
		bool replayLeft = true;
		bool sweepLeft = false;
		const bool replayRefused = failing("late-fail-replay", true, replayLeft);
		const bool sweepRefused = failing("late-fail-sweep", false, sweepLeft);
		const bool ok = ordered && replayRefused && !replayLeft && sweepRefused && sweepLeft;
		Report("late open serves only a swept store", ok,
		       std::to_string(served.load()) + " reads served, " + std::to_string(leaked.load()) +
			       " leaked; replay failure " + (replayRefused ? "served nothing" : "served") +
			       (replayLeft ? ", store left" : ", store removed") + "; sweep failure " +
			       (sweepRefused ? "served nothing" : "served") +
			       (sweepLeft ? ", store kept" : ", store gone"));
	}

	// Recovered names the copy it set aside only while that copy is there: a Clear takes it,
	// and the status is then plain Ok.
	{
		const std::string path = dbPath("recovered-clear");
		{
			std::ofstream out(fs::u8path(path), std::ios::binary);
			out << Garbage();
		}
		Launch launch(path, Chat::Retention::SevenDays, "L1");
		const bool named = launch.archive.Status() == Chat::ArchiveStatus::Recovered &&
				   launch.archive.StatusDetail().find(".corrupt-") != std::string::npos;
		launch.history.Clear();
		launch.archive.WaitIdle(kIdleWait);
		const bool ok = named && !AnyQuarantined(path) && launch.archive.Status() == Chat::ArchiveStatus::Ok &&
				launch.archive.StatusDetail().empty();
		Report("recovered clears with its copy", ok);
	}

	// A move to This session removes the copies set aside even when chat.db cannot be
	// opened (here a newer build's, which stays as it is).
	{
		const std::string path = dbPath("session-sweep");
		WriteWalDb(path, kNewerVersion);
		const std::string before = Bytes(path);
		const fs::path copy =
			fs::u8path(path).parent_path() /
			fs::u8path(fs::u8path(path).filename().u8string() + ".corrupt-" + TimeUtil::LocalFileStamp());
		std::ofstream(copy, std::ios::binary) << "x";
		Launch launch(path, Chat::Retention::SevenDays, "L1");
		const bool keptAtOpen = AnyQuarantined(path); // under 7 days old
		launch.history.SetRetention(Chat::Retention::Session);
		launch.archive.WaitIdle(kIdleWait);
		const bool ok = keptAtOpen && !AnyQuarantined(path) && Bytes(path) == before &&
				launch.archive.Status() == Chat::ArchiveStatus::NewerSchema;
		Report("session sweeps when the store cannot open", ok);
	}

	fs::remove_all(dir, ec);
}
