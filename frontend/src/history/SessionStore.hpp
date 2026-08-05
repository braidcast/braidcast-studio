#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Schema.hpp"

namespace History {

// The UI's read path over the history database, plus startup crash recovery.
// Separate from SessionRecorder so a slow query from a browsing user cannot
// stall the write path of a running broadcast.
//
// UI thread only.
class SessionStore {
public:
	SessionStore() = default;
	~SessionStore() = default;
	SessionStore(const SessionStore &) = delete;
	SessionStore &operator=(const SessionStore &) = delete;

	// Open the ORM connection over an already-migrated database. False leaves
	// the store unattached and every accessor empty -- history degrades to
	// unavailable, it never takes the app down.
	bool Attach(const std::string &path);
	void Detach();
	bool IsAttached() const { return storage_ != nullptr; }
	const std::string &LastError() const { return lastError_; }

	// A session row with no ended_at did not end -- it died. Stamp it
	// `crashed` and take its end time from its last health sample, falling
	// back to its start when it died before the first sample. Returns how many
	// were recovered, which is also the answer to "how often does this happen
	// to me".
	int RecoverCrashed();

	// Newest first. `skip` pages; both are named away from limit/offset
	// because those are sqlite_orm DSL functions the implementation calls.
	std::vector<Session> List(int count, int skip);
	std::unique_ptr<Session> Get(const std::string &id);
	std::vector<SessionDestination> DestinationsFor(const std::string &id);
	std::vector<SessionHealth> HealthFor(const std::string &id);

	// Cascades to destinations and health. Returns false if unattached.
	bool Remove(const std::string &id);

private:
	std::unique_ptr<Storage> storage_;
	std::string lastError_;
};

} // namespace History
