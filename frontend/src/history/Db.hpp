#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct sqlite3;

namespace History {

// Bumped by exactly one whenever a history migration is appended. The stored
// `user_version` pragma is compared against this at open.
inline constexpr int kCurrentSchemaVersion = 3;

// One step of a schema ladder: the version it stamps and the DDL that gets there.
struct Migration {
	int version;
	const char *sql;
};

// A database's whole migration ladder, oldest first. The last step's version is the
// schema this build writes. `name` only words the errors ("history database is newer
// than this build"). Each database file carries its own ladder, so a migration of one
// can never make an older build refuse another.
struct Ladder {
	const char *name;
	const Migration *steps;
	size_t count;

	template<size_t N>
	constexpr Ladder(const char *ladderName, const Migration (&ladderSteps)[N])
		: name(ladderName),
		  steps(ladderSteps),
		  count(N)
	{
	}

	constexpr int Current() const { return steps[count - 1].version; }
};

// history.db's ladder (Migrations.cpp).
extern const Ladder kHistoryLadder;

// Owns one SQLite database file: the connection, WAL mode, and the versioned
// `user_version` migration ladder handed to Open. Deliberately knows nothing about
// sessions or chat -- the schema belongs to whoever passes the ladder.
//
// Not thread-safe: every method must be called from one thread at a time. history.db's
// instance lives on the CEF UI thread; the chat archive's writer connection is opened on
// the main thread before its writer thread starts, and is used only by that thread after.
class Db {
public:
	Db() = default;
	~Db();
	Db(const Db &) = delete;
	Db &operator=(const Db &) = delete;

	// Open (creating if absent), set WAL, run every pending step of `ladder`. Returns
	// false and sets LastError() on failure; the caller degrades rather than aborting
	// startup. A file whose schema is newer than the ladder is refused before anything
	// is written to it (NewerSchema()).
	bool Open(const std::string &path, const Ladder &ladder);
	void Close();

	bool IsOpen() const { return handle_ != nullptr; }
	const std::string &LastError() const { return lastError_; }
	// The SQLite result code behind LastError() (SQLITE_NOTADB for a file that is not a
	// database, say); 0 when the failure was not SQLite's.
	int LastErrorCode() const { return lastErrorCode_; }
	// True after an Open refused a file written by a newer build.
	bool NewerSchema() const { return newerSchema_; }
	const std::string &Path() const { return path_; }

	// Schema version currently on disk. 0 for a database that has never been
	// migrated.
	int Version() const;

	// Run a statement for its effect. False sets LastError(). Intended for
	// migrations and tests -- feature code queries through sqlite_orm or its own
	// prepared statements, not here.
	bool Exec(const char *sql);

	// First column of the first row, or 0 if the query returns nothing.
	int64_t ScalarInt(const char *sql);

	// "wal" once Open() has succeeded. Exposed so a test can prove the mode
	// actually took rather than trusting the pragma was sent.
	std::string JournalMode();

	// Raw handle. Null when closed. sqlite_orm does not adopt one: make_storage
	// takes a filename and opens a second connection itself. `foreign_keys` and
	// `busy_timeout` are per-connection settings, so that second connection runs
	// with no busy timeout and gets foreign keys only from the amalgamation's
	// compiled-in SQLITE_DEFAULT_FOREIGN_KEYS=1.
	sqlite3 *Handle() const { return handle_; }

private:
	// False with LastError() set when there is no connection.
	bool RequireOpen();

	// Records the connection's current error as LastError()/LastErrorCode().
	void RecordError();

	// The stored `user_version`, false with LastError() set when it cannot be read --
	// which is how a file that is not a database first shows itself.
	bool ReadVersion(int &version);

	bool Migrate(const Ladder &ladder);

	// Rolls back the migration transaction and fails with `reason`. Takes it by
	// value so that RollbackWith(lastError_) snapshots the cause before the
	// rollback itself overwrites lastError_.
	bool RollbackWith(std::string reason);

	sqlite3 *handle_ = nullptr;
	std::string path_;
	std::string lastError_;
	int lastErrorCode_ = 0;
	bool newerSchema_ = false;
};

} // namespace History
