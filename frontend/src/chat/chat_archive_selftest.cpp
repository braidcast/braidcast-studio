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
		// Each row's id and `deleted` mark, oldest first, as ReadOlder serves them.
		const auto marks = [&] {
			const std::optional<std::vector<json>> rows = archive.ReadOlder(UINT64_MAX, 10, Feed::Filter{});
			if (!rows) {
				return std::string("unreadable");
			}
			std::string out;
			for (auto it = rows->rbegin(); it != rows->rend(); ++it) {
				out += (out.empty() ? "" : ",") + it->value("id", "") + "=" + it->value("deleted", "");
			}
			return out;
		};
		enqueue(1);
		enqueue(2);
		enqueue(3);
		archive.WaitIdle(kIdleWait);
		archive.HoldWrites(true);
		archive.EnqueueRedact(
			{OAuth::DestinationKey(kTwitch), Chat::ModerationAction::ClearUser, "", "author-x", 3});
		enqueue(4);
		const std::string pending = marks();
		archive.HoldWrites(false);
		archive.WaitIdle(kIdleWait);
		const std::string committed = marks();
		const std::string stored = Column(path, "SELECT msg_id || '=' || deleted FROM messages ORDER BY seq");
		archive.Shutdown();
		const bool ok = pending == "b1=user,b2=user,b3=" && committed == "b1=user,b2=user,b3=,b4=" &&
				stored == committed;
		Report("redaction bound", ok, pending + " | " + committed + " | " + stored);
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
				detail.find(".corrupt-") == std::string::npos &&
				Scalar(path, "SELECT count(*) FROM messages") == 1;
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

	fs::remove_all(dir, ec);
}
