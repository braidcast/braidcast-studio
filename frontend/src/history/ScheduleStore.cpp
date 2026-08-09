#include "ScheduleStore.hpp"

#include "util/time_util.hpp"
#include "uuid_util.hpp"

namespace History {

using namespace sqlite_orm;

bool ScheduleStore::Attach(const std::string &path)
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

void ScheduleStore::Detach()
{
	storage_.reset();
}

std::vector<ScheduleEntryWithDestinations> ScheduleStore::ListRange(int64_t fromMs, int64_t toMs)
{
	if (!storage_) {
		return {};
	}
	try {
		auto entries = storage_->get_all<ScheduleEntry>(where(c(&ScheduleEntry::startsAt) >= fromMs and
								      c(&ScheduleEntry::startsAt) < toMs),
								order_by(&ScheduleEntry::startsAt).asc());

		// One query for every destination in the range rather than one per
		// entry: a busy month is a few dozen entries, and the per-entry form
		// would put that many round trips behind every calendar paint.
		std::vector<std::string> ids;
		ids.reserve(entries.size());
		for (const ScheduleEntry &e : entries) {
			ids.push_back(e.id);
		}

		std::vector<ScheduleEntryWithDestinations> out;
		out.reserve(entries.size());
		for (ScheduleEntry &e : entries) {
			out.push_back({std::move(e), {}});
		}
		if (ids.empty()) {
			lastError_.clear();
			return out;
		}

		const auto dests =
			storage_->get_all<ScheduleDestination>(where(in(&ScheduleDestination::scheduleId, ids)));
		for (const ScheduleDestination &d : dests) {
			for (ScheduleEntryWithDestinations &row : out) {
				if (row.entry.id == d.scheduleId) {
					row.destinations.push_back(d);
					break;
				}
			}
		}
		lastError_.clear();
		return out;
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return {};
	}
}

std::unique_ptr<ScheduleEntry> ScheduleStore::Get(const std::string &id)
{
	if (!storage_) {
		return nullptr;
	}
	try {
		return storage_->get_pointer<ScheduleEntry>(id);
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return nullptr;
	}
}

std::vector<ScheduleDestination> ScheduleStore::DestinationsFor(const std::string &id)
{
	if (!storage_) {
		return {};
	}
	try {
		return storage_->get_all<ScheduleDestination>(where(c(&ScheduleDestination::scheduleId) == id));
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return {};
	}
}

bool ScheduleStore::Create(ScheduleEntry &entry, const std::vector<ScheduleDestination> &destinations)
{
	if (!storage_) {
		return false;
	}
	if (entry.id.empty()) {
		entry.id = UuidUtil::New();
	}
	const int64_t now = TimeUtil::NowMs();
	entry.createdAt = now;
	entry.updatedAt = now;
	try {
		// The entry and its destinations land together or not at all: an entry
		// with no destinations is indistinguishable from one the user has not
		// finished, and the runner would arm it and find nothing to go live to.
		storage_->transaction([&] {
			storage_->replace(entry);
			for (ScheduleDestination d : destinations) {
				if (d.id.empty()) {
					d.id = UuidUtil::New();
				}
				d.scheduleId = entry.id;
				d.createdAt = now;
				d.updatedAt = now;
				storage_->replace(d);
			}
			return true;
		});
		lastError_.clear();
		return true;
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return false;
	}
}

bool ScheduleStore::Update(const ScheduleEntry &entry, const std::vector<ScheduleDestination> &destinations)
{
	if (!storage_) {
		return false;
	}
	const int64_t now = TimeUtil::NowMs();
	try {
		storage_->transaction([&] {
			// updated_at is the trigger's to set; passing the old value here
			// and letting the trigger overwrite it keeps one writer for that
			// column.
			storage_->update(entry);
			storage_->remove_all<ScheduleDestination>(
				where(c(&ScheduleDestination::scheduleId) == entry.id));
			for (ScheduleDestination d : destinations) {
				d.id = UuidUtil::New();
				d.scheduleId = entry.id;
				d.createdAt = now;
				d.updatedAt = now;
				storage_->replace(d);
			}
			return true;
		});
		lastError_.clear();
		return true;
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return false;
	}
}

bool ScheduleStore::SetState(const std::string &id, const std::string &state)
{
	if (!storage_) {
		return false;
	}
	try {
		storage_->update_all(set(assign(&ScheduleEntry::state, state)), where(c(&ScheduleEntry::id) == id));
		lastError_.clear();
		return true;
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return false;
	}
}

bool ScheduleStore::Remove(const std::string &id)
{
	if (!storage_) {
		return false;
	}
	try {
		storage_->transaction([&] {
			// Deleting the plan must not delete the record of the broadcast it
			// planned. The session keeps its row and loses only the link, which
			// is why this is not left to ON DELETE CASCADE.
			storage_->update_all(set(assign(&Session::scheduleId, std::optional<std::string>{})),
					     where(c(&Session::scheduleId) == id));
			storage_->remove<ScheduleEntry>(id);
			return true;
		});
		lastError_.clear();
		return true;
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return false;
	}
}

int ScheduleStore::SweepMissed(int64_t nowMs)
{
	if (!storage_) {
		return 0;
	}
	try {
		// Only planned and armed can be missed. An entry that reached `live`
		// happened, and one already `done`, `canceled` or `missed` is settled --
		// re-marking those would rewrite history every tick.
		const auto stale = storage_->get_all<ScheduleEntry>(
			where(c(&ScheduleEntry::startsAt) < nowMs and
			      (c(&ScheduleEntry::state) == std::string(ScheduleState::kPlanned) or
			       c(&ScheduleEntry::state) == std::string(ScheduleState::kArmed))));
		for (const ScheduleEntry &e : stale) {
			storage_->update_all(set(assign(&ScheduleEntry::state, std::string(ScheduleState::kMissed))),
					     where(c(&ScheduleEntry::id) == e.id));
		}
		lastError_.clear();
		return (int)stale.size();
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return 0;
	}
}

} // namespace History
