#include "Db.hpp"

#include <sqlite3.h>

namespace History {
namespace {

// Steps a query for its first column of its first row. Reports only whether the
// statement could be prepared, so the const Version() and the error-recording
// ScalarInt() can share one implementation without sharing error handling.
bool StepScalar(sqlite3 *handle, const char *sql, int64_t &value)
{
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return false;
	}
	value = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
	sqlite3_finalize(stmt);
	return true;
}

} // namespace

Db::~Db()
{
	Close();
}

bool Db::Open(const std::string &path, const Ladder &ladder)
{
	Close();
	newerSchema_ = false;
	lastErrorCode_ = 0;
	const int rc = sqlite3_open_v2(path.c_str(), &handle_,
				       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
	if (rc != SQLITE_OK) {
		lastError_ = handle_ ? sqlite3_errmsg(handle_) : "out of memory opening the database";
		lastErrorCode_ = rc;
		Close();
		return false;
	}
	path_ = path;
	lastError_.clear();

	// `foreign_keys = ON` repeats what SQLITE_DEFAULT_FOREIGN_KEYS=1 already
	// gives the amalgamation. The duplication is deliberate: losing that build
	// definition would stop every ON DELETE CASCADE from firing, and the symptom
	// is orphan rows accumulating silently rather than anything failing.
	if (!Exec("PRAGMA foreign_keys = ON") || !Exec("PRAGMA busy_timeout = 3000")) {
		Close();
		return false;
	}
	// A newer build's file is refused before the journal-mode switch below, which
	// rewrites the header of a file not already in WAL: a downgrade must leave the
	// file exactly as the newer build wrote it. Migrate re-checks inside its
	// transaction, where the answer cannot go stale.
	int version = 0;
	if (!ReadVersion(version)) {
		Close();
		return false;
	}
	if (version > ladder.Current()) {
		newerSchema_ = true;
		lastError_ = std::string(ladder.name) + " database is newer than this build";
		Close();
		return false;
	}
	if (!Exec("PRAGMA journal_mode = WAL")) {
		Close();
		return false;
	}
	if (!Migrate(ladder)) {
		Close();
		return false;
	}
	return true;
}

void Db::Close()
{
	if (handle_) {
		sqlite3_close_v2(handle_);
		handle_ = nullptr;
	}
	path_.clear();
}

int Db::Version() const
{
	int64_t version = 0;
	if (!handle_ || !StepScalar(handle_, "PRAGMA user_version", version)) {
		return 0;
	}
	return static_cast<int>(version);
}

bool Db::ReadVersion(int &version)
{
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(handle_, "PRAGMA user_version", -1, &stmt, nullptr) != SQLITE_OK) {
		RecordError();
		return false;
	}
	const int rc = sqlite3_step(stmt);
	version = rc == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
	sqlite3_finalize(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		RecordError();
		return false;
	}
	return true;
}

void Db::RecordError()
{
	lastError_ = sqlite3_errmsg(handle_);
	lastErrorCode_ = sqlite3_errcode(handle_);
}

bool Db::RequireOpen()
{
	if (handle_) {
		return true;
	}
	lastError_ = "database is not open";
	return false;
}

bool Db::Exec(const char *sql)
{
	if (!RequireOpen()) {
		return false;
	}
	char *err = nullptr;
	if (sqlite3_exec(handle_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
		lastErrorCode_ = sqlite3_errcode(handle_);
		lastError_ = err ? err : sqlite3_errmsg(handle_);
		sqlite3_free(err);
		return false;
	}
	return true;
}

int64_t Db::ScalarInt(const char *sql)
{
	if (!RequireOpen()) {
		return 0;
	}
	int64_t value = 0;
	if (!StepScalar(handle_, sql, value)) {
		lastError_ = sqlite3_errmsg(handle_);
		return 0;
	}
	return value;
}

std::string Db::JournalMode()
{
	if (!RequireOpen()) {
		return {};
	}
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(handle_, "PRAGMA journal_mode", -1, &stmt, nullptr) != SQLITE_OK) {
		lastError_ = sqlite3_errmsg(handle_);
		return {};
	}
	std::string mode;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		if (const unsigned char *text = sqlite3_column_text(stmt, 0)) {
			mode = reinterpret_cast<const char *>(text);
		}
	}
	sqlite3_finalize(stmt);
	return mode;
}

} // namespace History
