#pragma once

#include <sqlite_orm/sqlite_orm.h>

#include <cstdint>
#include <optional>
#include <string>

namespace History {

// The typed view of schema v1. Migrations.cpp owns the DDL that actually creates
// these tables; this is a second description of the same thing, so every column
// name, nullability and default below has to match that DDL character for
// character -- sqlite_orm resolves columns by name and compares nullability,
// default presence and primary-key flag against the live table, so a divergence
// here does not fail loudly, it silently addresses a column that is not there.
// test_orm_schema_matches_migrated_schema is what holds the two in agreement.

struct Session {
	std::string id;
	int64_t createdAt = 0;
	int64_t updatedAt = 0;
	int64_t startedAt = 0;
	// Empty until the session ends. `ended_at IS NULL` is the whole
	// crash-detection predicate -- a sentinel zero would read as "ended at the
	// epoch" and make every recovered session look finished.
	std::optional<int64_t> endedAt;
	std::optional<std::string> endReason;
	std::optional<std::string> scheduleId;
	std::string title;
	// JSON array of canvas uuids.
	std::string canvasUuids = "[]";
	std::optional<std::string> thumbPath;
};

struct SessionDestination {
	std::string id;
	int64_t createdAt = 0;
	int64_t updatedAt = 0;
	std::string sessionId;
	std::string profileId;
	std::string platform;
	std::string accountLabel;
	std::string title;
	std::string category;
	// JSON array of tag strings.
	std::string tags = "[]";
	std::string finalState;
	std::string error;
};

// A planned broadcast. `announce` and `autoStart` are 0/1 rather than bool so the
// struct keeps matching the DDL column for column, which is the property the
// comment above asks of this file; the bridge is where they become JSON booleans.
struct ScheduleEntry {
	std::string id;
	int64_t createdAt = 0;
	int64_t updatedAt = 0;
	int64_t startsAt = 0;
	std::string title;
	int64_t durationMin = 60;
	int64_t announce = 0;
	int64_t autoStart = 0;
	// Both reserved: recurrence is out of scope for v1, remote_ref is written
	// only once announcing exists. Neither is read by anything in this phase.
	std::optional<std::string> recurrence;
	std::optional<std::string> remoteRef;
	// planned | armed | live | done | missed | canceled, enforced by a CHECK in
	// the migration rather than here.
	std::string state = "planned";
};

struct ScheduleDestination {
	std::string id;
	int64_t createdAt = 0;
	int64_t updatedAt = 0;
	std::string scheduleId;
	std::string profileId;
	std::string title;
	std::string category;
	// JSON array of tag strings.
	std::string tags = "[]";
};

struct SessionHealth {
	// Assigned by SQLite on insert; 0 stands for "not stored yet".
	int64_t id = 0;
	std::string sessionId;
	int64_t t = 0;
	int64_t bitrateKbps = 0;
	int64_t droppedFrames = 0;
	double congestionPct = 0.0;
	int64_t encodeSkipped = 0;
	double cpuPct = 0.0;
};

inline auto MakeStorage(const std::string &path)
{
	using namespace sqlite_orm;
	return make_storage(
		path,
		make_table("sessions", make_column("id", &Session::id, primary_key()),
			   make_column("created_at", &Session::createdAt),
			   make_column("updated_at", &Session::updatedAt),
			   make_column("started_at", &Session::startedAt), make_column("ended_at", &Session::endedAt),
			   make_column("end_reason", &Session::endReason),
			   make_column("schedule_id", &Session::scheduleId),
			   make_column("title", &Session::title, default_value(std::string{})),
			   make_column("canvas_uuids", &Session::canvasUuids, default_value(std::string{"[]"})),
			   make_column("thumb_path", &Session::thumbPath)),
		make_table("session_destinations", make_column("id", &SessionDestination::id, primary_key()),
			   make_column("created_at", &SessionDestination::createdAt),
			   make_column("updated_at", &SessionDestination::updatedAt),
			   make_column("session_id", &SessionDestination::sessionId),
			   make_column("profile_id", &SessionDestination::profileId, default_value(std::string{})),
			   make_column("platform", &SessionDestination::platform, default_value(std::string{})),
			   make_column("account_label", &SessionDestination::accountLabel,
				       default_value(std::string{})),
			   make_column("title", &SessionDestination::title, default_value(std::string{})),
			   make_column("category", &SessionDestination::category, default_value(std::string{})),
			   make_column("tags", &SessionDestination::tags, default_value(std::string{"[]"})),
			   make_column("final_state", &SessionDestination::finalState, default_value(std::string{})),
			   make_column("error", &SessionDestination::error, default_value(std::string{}))),
		make_table("session_health", make_column("id", &SessionHealth::id, primary_key().autoincrement()),
			   make_column("session_id", &SessionHealth::sessionId), make_column("t", &SessionHealth::t),
			   make_column("bitrate_kbps", &SessionHealth::bitrateKbps, default_value(int64_t{0})),
			   make_column("dropped_frames", &SessionHealth::droppedFrames, default_value(int64_t{0})),
			   make_column("congestion_pct", &SessionHealth::congestionPct, default_value(0.0)),
			   make_column("encode_skipped", &SessionHealth::encodeSkipped, default_value(int64_t{0})),
			   make_column("cpu_pct", &SessionHealth::cpuPct, default_value(0.0))),
		make_table("schedule", make_column("id", &ScheduleEntry::id, primary_key()),
			   make_column("created_at", &ScheduleEntry::createdAt),
			   make_column("updated_at", &ScheduleEntry::updatedAt),
			   make_column("starts_at", &ScheduleEntry::startsAt),
			   make_column("title", &ScheduleEntry::title, default_value(std::string{})),
			   make_column("duration_min", &ScheduleEntry::durationMin, default_value(int64_t{60})),
			   make_column("announce", &ScheduleEntry::announce, default_value(int64_t{0})),
			   make_column("auto_start", &ScheduleEntry::autoStart, default_value(int64_t{0})),
			   make_column("recurrence", &ScheduleEntry::recurrence),
			   make_column("remote_ref", &ScheduleEntry::remoteRef),
			   make_column("state", &ScheduleEntry::state, default_value(std::string{"planned"}))),
		make_table("schedule_destinations", make_column("id", &ScheduleDestination::id, primary_key()),
			   make_column("created_at", &ScheduleDestination::createdAt),
			   make_column("updated_at", &ScheduleDestination::updatedAt),
			   make_column("schedule_id", &ScheduleDestination::scheduleId),
			   make_column("profile_id", &ScheduleDestination::profileId, default_value(std::string{})),
			   make_column("title", &ScheduleDestination::title, default_value(std::string{})),
			   make_column("category", &ScheduleDestination::category, default_value(std::string{})),
			   make_column("tags", &ScheduleDestination::tags, default_value(std::string{"[]"}))));
}

// sqlite_orm's storage type is a deduced tuple of every table declaration and
// cannot be spelled out.
using Storage = decltype(MakeStorage(std::string{}));

} // namespace History
