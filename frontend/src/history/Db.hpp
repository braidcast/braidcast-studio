#pragma once

#include <cstdint>
#include <string>

struct sqlite3;

namespace History {

// Bumped by exactly one whenever a migration is appended. The stored
// `user_version` pragma is compared against this at open.
inline constexpr int kCurrentSchemaVersion = 1;

// Owns the history database file: the connection, WAL mode, and the versioned
// `user_version` migration ladder. Deliberately knows nothing about sessions --
// the other stores are expected to migrate onto this later, so nothing
// history-specific belongs here.
//
// Every method must be called on the CEF UI thread.
class Db {
public:
	Db() = default;
	~Db();
	Db(const Db &) = delete;
	Db &operator=(const Db &) = delete;

	// Open (creating if absent), set WAL, run every pending migration. Returns
	// false and sets LastError() on failure; the caller degrades rather than
	// aborting startup.
	bool Open(const std::string &path);
	void Close();

	bool IsOpen() const { return handle_ != nullptr; }
	const std::string &LastError() const { return lastError_; }
	const std::string &Path() const { return path_; }

	// Schema version currently on disk. 0 for a database that has never been
	// migrated.
	int Version() const;

	// Run a statement for its effect. False sets LastError(). Intended for
	// migrations and tests -- feature code queries through sqlite_orm, not here.
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

	bool Migrate();

	// Rolls back the migration transaction and fails with `reason`. Takes it by
	// value so that RollbackWith(lastError_) snapshots the cause before the
	// rollback itself overwrites lastError_.
	bool RollbackWith(std::string reason);

	sqlite3 *handle_ = nullptr;
	std::string path_;
	std::string lastError_;
};

} // namespace History
