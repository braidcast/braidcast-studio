#pragma once

#include <string>

struct sqlite3;

namespace History {

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

private:
	sqlite3 *handle_ = nullptr;
	std::string path_;
	std::string lastError_;
};

} // namespace History
