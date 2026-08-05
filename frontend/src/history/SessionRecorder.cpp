#include "SessionRecorder.hpp"

#include <nlohmann/json.hpp>

#include <util/platform.h>
#include <util/util.hpp>

#include "../util/time_util.hpp"

namespace History {

using namespace sqlite_orm;

namespace {

std::string NewUuid()
{
	// The same kind of key the rest of this codebase uses for durable things
	// (canvas uuid, binding uuid): stable, non-reassignable, and referenced by
	// thumbnail files on disk.
	BPtr<char> id = os_generate_uuid();
	return id ? std::string(id) : std::string();
}

std::string ToJsonArray(const std::vector<std::string> &values)
{
	return nlohmann::json(values).dump();
}

// A counter that went backwards means the baseline was reset underneath us
// (stats.reset), so the current value IS the delta.
int64_t Delta(int64_t now, int64_t before)
{
	return now >= before ? now - before : now;
}

} // namespace

bool SessionRecorder::Attach(const std::string &path)
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

void SessionRecorder::Detach()
{
	storage_.reset();
	currentId_.clear();
	bindings_.clear();
	havePersisted_ = false;
}

std::string SessionRecorder::Begin(const SessionStart &start)
{
	if (!storage_ || IsRecording()) {
		return {};
	}
	try {
		Session s;
		s.id = NewUuid();
		s.createdAt = TimeUtil::NowMs();
		s.updatedAt = s.createdAt;
		s.startedAt = start.startedAtMs;
		if (!start.scheduleId.empty()) {
			s.scheduleId = start.scheduleId;
		}
		s.title = start.title;
		s.canvasUuids = ToJsonArray(start.canvasUuids);
		storage_->replace(s);

		for (const DestinationRecord &d : start.destinations) {
			SessionDestination row;
			row.id = NewUuid();
			row.createdAt = s.createdAt;
			row.updatedAt = s.createdAt;
			row.sessionId = s.id;
			row.profileId = d.profileId;
			row.platform = d.platform;
			row.accountLabel = d.accountLabel;
			row.title = d.title;
			row.category = d.category;
			row.tags = ToJsonArray(d.tags);
			bindings_[d.bindingUuid] = row.id;
			storage_->replace(row);
		}

		currentId_ = s.id;
		startedAtMs_ = start.startedAtMs;
		havePersisted_ = false;
		lastError_.clear();
		return currentId_;
	} catch (const std::exception &e) {
		lastError_ = e.what();
		return {};
	}
}

void SessionRecorder::OnSample(const HealthSample &sample)
{
	if (!storage_ || !IsRecording()) {
		return;
	}
	if (havePersisted_ && sample.tMs - lastPersisted_.tMs < kHealthIntervalMs) {
		return;
	}
	try {
		SessionHealth row;
		row.sessionId = currentId_;
		row.t = sample.tMs;
		row.bitrateKbps = sample.bitrateKbps;
		row.congestionPct = sample.congestionPct;
		row.cpuPct = sample.cpuPct;
		if (havePersisted_) {
			row.droppedFrames =
				Delta(sample.cumulativeDroppedFrames, lastPersisted_.cumulativeDroppedFrames);
			row.encodeSkipped =
				Delta(sample.cumulativeEncodeSkipped, lastPersisted_.cumulativeEncodeSkipped);
		} else {
			row.droppedFrames = sample.cumulativeDroppedFrames;
			row.encodeSkipped = sample.cumulativeEncodeSkipped;
		}
		storage_->insert(row);
		lastPersisted_ = sample;
		havePersisted_ = true;
	} catch (const std::exception &e) {
		// Disk full mid-broadcast lands here. Drop the sample and keep
		// streaming: the recorder must never take down the stream. The baseline
		// is left where it was, so the next row that does land carries the
		// frames from the dropped interval too rather than losing them.
		lastError_ = e.what();
	}
}

void SessionRecorder::OnDestinationEnded(const std::string &bindingUuid, const std::string &finalState,
					 const std::string &error)
{
	if (!storage_ || !IsRecording()) {
		return;
	}
	const auto it = bindings_.find(bindingUuid);
	if (it == bindings_.end()) {
		return;
	}
	try {
		storage_->update_all(set(assign(&SessionDestination::finalState, finalState),
					 assign(&SessionDestination::error, error)),
				     where(is_equal(&SessionDestination::id, it->second)));
	} catch (const std::exception &e) {
		lastError_ = e.what();
	}
}

void SessionRecorder::End(int64_t endedAtMs, const std::string &reason)
{
	if (!storage_ || !IsRecording()) {
		return;
	}
	try {
		storage_->update_all(set(assign(&Session::endedAt, std::make_optional(endedAtMs)),
					 assign(&Session::endReason, std::make_optional(reason))),
				     where(is_equal(&Session::id, currentId_)));
	} catch (const std::exception &e) {
		lastError_ = e.what();
	}
	currentId_.clear();
	bindings_.clear();
	havePersisted_ = false;
}

void SessionRecorder::SetThumbnail(const std::string &relPath)
{
	if (!storage_ || !IsRecording()) {
		return;
	}
	try {
		storage_->update_all(set(assign(&Session::thumbPath, std::make_optional(relPath))),
				     where(is_equal(&Session::id, currentId_)));
	} catch (const std::exception &e) {
		lastError_ = e.what();
	}
}

} // namespace History
