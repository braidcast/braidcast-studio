#include "SessionStore.hpp"

namespace History {

using namespace sqlite_orm;

bool SessionStore::Attach(const std::string &path)
{
	Detach();
	try {
		storage_ = std::make_unique<Storage>(MakeStorage(path));
		storage_->busy_timeout(3000);
		lastError_.clear();
		return true;
	} catch (const std::exception &e) {
		lastError_ = e.what();
		storage_.reset();
		return false;
	}
}

void SessionStore::Detach()
{
	storage_.reset();
}

int SessionStore::RecoverCrashed()
{
	if (!storage_) {
		return 0;
	}
	int recovered = 0;
	try {
		const auto dead = storage_->get_all<Session>(where(is_null(&Session::endedAt)));
		for (const Session &s : dead) {
			// The end time is the last thing we know actually happened, not now:
			// a crash at 21:40 that is not recovered until Tuesday must not claim
			// the session ran until Tuesday. With no samples at all the session
			// died before the first tick, so its start is the only honest answer.
			const auto lastSample =
				storage_->max(&SessionHealth::t, where(is_equal(&SessionHealth::sessionId, s.id)));
			const int64_t endedAt = lastSample ? *lastSample : s.startedAt;
			storage_->update_all(
				set(assign(&Session::endedAt, std::make_optional(endedAt)),
				    assign(&Session::endReason, std::make_optional(std::string("crashed")))),
				where(is_equal(&Session::id, s.id)));
			recovered++;
		}
		lastError_.clear();
	} catch (const std::exception &e) {
		lastError_ = e.what();
	}
	return recovered;
}

std::vector<Session> SessionStore::List(int count, int skip)
{
	if (!storage_) {
		return {};
	}
	try {
		return storage_->get_all<Session>(order_by(&Session::startedAt).desc(), limit(count, offset(skip)));
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return {};
	}
}

std::unique_ptr<Session> SessionStore::Get(const std::string &id)
{
	if (!storage_) {
		return nullptr;
	}
	try {
		return storage_->get_pointer<Session>(id);
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return nullptr;
	}
}

std::vector<SessionDestination> SessionStore::DestinationsFor(const std::string &id)
{
	if (!storage_) {
		return {};
	}
	try {
		return storage_->get_all<SessionDestination>(where(is_equal(&SessionDestination::sessionId, id)));
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return {};
	}
}

std::vector<SessionHealth> SessionStore::HealthFor(const std::string &id)
{
	if (!storage_) {
		return {};
	}
	try {
		return storage_->get_all<SessionHealth>(where(is_equal(&SessionHealth::sessionId, id)),
							order_by(&SessionHealth::t));
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return {};
	}
}

bool SessionStore::Remove(const std::string &id)
{
	if (!storage_) {
		return false;
	}
	try {
		// The children go with it through the schema's ON DELETE CASCADE, which
		// only fires because the amalgamation is built SQLITE_DEFAULT_FOREIGN_KEYS.
		storage_->remove<Session>(id);
		lastError_.clear();
		return true;
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return false;
	}
}

} // namespace History
