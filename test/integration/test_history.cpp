#include "harness.hpp"

#include "history/Db.hpp"
#include "history/Schema.hpp"
#include "history/SessionRecorder.hpp"
#include "history/SessionStore.hpp"

#include <string>

// cmocka requires these in this order before cmocka.h, and on MSVC cmocka.h
// macroizes `inline`, which the C++ standard library rejects -- so every other
// header this file needs has to come above it. Wrap cmocka.h in extern "C" so its
// symbols get C linkage in this C++ TU (its own guard does not apply here).
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
extern "C" {
#include <cmocka.h>
}

// A database path inside the test's temp area. Never the user's real config
// directory -- smoke and test runs share %APPDATA% through the rundir junction,
// so a test that resolved the real path would eat real history.
static std::string TempDbPath(const char *name)
{
	return Harness::TempDir() + "/" + name;
}

// The one session, and the one destination hanging off it, that the tests below
// build on. Both tables' NOT NULL columns without defaults have to be named.
static bool SeedSession(History::Db &db)
{
	return db.Exec("INSERT INTO sessions (id, created_at, updated_at, started_at, title, canvas_uuids) "
		       "VALUES ('s1', 1, 1, 1000, 'first', '[]')");
}

static bool SeedDestination(History::Db &db)
{
	return db.Exec("INSERT INTO session_destinations (id, created_at, updated_at, session_id, platform) "
		       "VALUES ('d1', 1, 1, 's1', 'twitch')");
}

static void test_open_creates_database(void **state)
{
	(void)state;
	History::Db db;
	const std::string path = TempDbPath("open_creates.db");
	assert_true(db.Open(path));
	assert_true(db.IsOpen());
	db.Close();
	assert_false(db.IsOpen());
}

static void test_open_sets_wal_and_version(void **state)
{
	(void)state;
	History::Db db;
	assert_true(db.Open(TempDbPath("wal_and_version.db")));
	assert_int_equal(db.Version(), History::kCurrentSchemaVersion);
	assert_string_equal(db.JournalMode().c_str(), "wal");
	db.Close();
}

static void test_migration_is_idempotent(void **state)
{
	(void)state;
	const std::string path = TempDbPath("idempotent.db");
	{
		History::Db db;
		assert_true(db.Open(path));
		assert_true(SeedSession(db));
		db.Close();
	}
	{
		History::Db db;
		assert_true(db.Open(path));
		assert_int_equal(db.Version(), History::kCurrentSchemaVersion);
		assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM sessions"), 1);
		db.Close();
	}
}

// A database written by a newer build has to be refused rather than opened and
// written through a schema this binary does not know. Reachable in practice: the
// portable rundir's config directory is a junction to the installed build's, so
// two versions can meet over one file.
static void test_newer_database_is_refused(void **state)
{
	(void)state;
	const std::string path = TempDbPath("newer.db");
	{
		History::Db db;
		assert_true(db.Open(path));
		assert_true(db.Exec("PRAGMA user_version = 999"));
		db.Close();
	}
	History::Db db;
	assert_false(db.Open(path));
	assert_false(db.IsOpen());
	assert_string_equal(db.LastError().c_str(), "history database is newer than this build");
}

// Asserts that updated_at moved rather than that it reached a particular value,
// so the test carries no dependency on the clock the trigger reads.
static void test_updated_at_trigger_fires(void **state)
{
	(void)state;
	History::Db db;
	assert_true(db.Open(TempDbPath("trigger.db")));
	assert_true(SeedSession(db));
	assert_true(SeedDestination(db));
	assert_true(db.Exec("UPDATE sessions SET title = 'renamed' WHERE id = 's1'"));
	assert_true(db.ScalarInt("SELECT updated_at FROM sessions WHERE id = 's1'") > 1);
	assert_true(db.Exec("UPDATE session_destinations SET title = 'renamed' WHERE id = 'd1'"));
	assert_true(db.ScalarInt("SELECT updated_at FROM session_destinations WHERE id = 'd1'") > 1);
}

static void test_delete_cascades_to_children(void **state)
{
	(void)state;
	History::Db db;
	assert_true(db.Open(TempDbPath("cascade.db")));
	assert_true(SeedSession(db));
	assert_true(SeedDestination(db));
	assert_true(db.Exec("INSERT INTO session_health (session_id, t, bitrate_kbps, dropped_frames, "
			    "congestion_pct, encode_skipped, cpu_pct) VALUES ('s1', 1010, 6000, 0, 0.0, 0, 12.5)"));
	// Both children counted before the delete: ScalarInt returns 0 for a query
	// that failed as well as for one that found nothing, so without this the
	// assertions below would pass against rows that were never inserted.
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM session_health"), 1);
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM session_destinations"), 1);
	assert_true(db.Exec("DELETE FROM sessions WHERE id = 's1'"));
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM session_health"), 0);
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM session_destinations"), 0);
}

// The DDL in Migrations.cpp and the declaration in Schema.hpp describe one
// schema twice. sqlite_orm compares them on column name, nullability,
// primary-key flag and whether a default exists, so this catches a renamed,
// missing, extra or differently-nullable column. It does not compare declared
// types or the default's value.
static void test_orm_schema_matches_migrated_schema(void **state)
{
	(void)state;
	const std::string path = TempDbPath("orm_match.db");
	{
		History::Db db;
		assert_true(db.Open(path));
		db.Close();
	}
	auto storage = History::MakeStorage(path);
	for (const auto &entry : storage.sync_schema_simulate(true)) {
		if (entry.second != sqlite_orm::sync_schema_result::already_in_sync) {
			fail_msg("table '%s' drifted from the migrated schema", entry.first.c_str());
		}
	}
}

static void test_recovery_marks_unended_session_crashed(void **state)
{
	(void)state;
	const std::string path = TempDbPath("recovery.db");
	History::Db db;
	assert_true(db.Open(path));
	assert_true(db.Exec("INSERT INTO sessions (id, created_at, updated_at, started_at, title, canvas_uuids) "
			    "VALUES ('s1', 1, 1, 1000, 'died', '[]')"));
	assert_true(db.Exec("INSERT INTO session_health (session_id, t, bitrate_kbps, dropped_frames, "
			    "congestion_pct, encode_skipped, cpu_pct) VALUES ('s1', 5000, 6000, 0, 0.0, 0, 10.0)"));
	assert_true(db.Exec("INSERT INTO session_health (session_id, t, bitrate_kbps, dropped_frames, "
			    "congestion_pct, encode_skipped, cpu_pct) VALUES ('s1', 9000, 6000, 0, 0.0, 0, 10.0)"));

	History::SessionStore store;
	assert_true(store.Attach(path));
	assert_int_equal(store.RecoverCrashed(), 1);

	const auto sessions = store.List(10, 0);
	assert_int_equal((int)sessions.size(), 1);
	assert_string_equal(sessions[0].endReason.value().c_str(), "crashed");
	// The end time is the last health sample, not "now" -- a crash at 21:40 that
	// is recovered on Tuesday must not claim it ran until Tuesday.
	assert_true(sessions[0].endedAt.has_value());
	assert_int_equal((int)*sessions[0].endedAt, 9000);
}

static void test_recovery_without_health_falls_back_to_start(void **state)
{
	(void)state;
	const std::string path = TempDbPath("recovery_nohealth.db");
	History::Db db;
	assert_true(db.Open(path));
	assert_true(db.Exec("INSERT INTO sessions (id, created_at, updated_at, started_at, title, canvas_uuids) "
			    "VALUES ('s1', 1, 1, 1000, 'died fast', '[]')"));

	History::SessionStore store;
	assert_true(store.Attach(path));
	assert_int_equal(store.RecoverCrashed(), 1);
	const auto sessions = store.List(10, 0);
	assert_true(sessions[0].endedAt.has_value());
	assert_int_equal((int)*sessions[0].endedAt, 1000);
}

static void test_recovery_leaves_clean_sessions_alone(void **state)
{
	(void)state;
	const std::string path = TempDbPath("recovery_clean.db");
	History::Db db;
	assert_true(db.Open(path));
	assert_true(db.Exec("INSERT INTO sessions (id, created_at, updated_at, started_at, ended_at, end_reason, "
			    "title, canvas_uuids) VALUES ('s1', 1, 1, 1000, 4000, 'ended', 'fine', '[]')"));

	History::SessionStore store;
	assert_true(store.Attach(path));
	assert_int_equal(store.RecoverCrashed(), 0);
	assert_string_equal(store.List(10, 0)[0].endReason.value().c_str(), "ended");
}

static void test_list_is_newest_first_and_paged(void **state)
{
	(void)state;
	const std::string path = TempDbPath("list_paged.db");
	History::Db db;
	assert_true(db.Open(path));
	for (int i = 1; i <= 5; i++) {
		char sql[256];
		snprintf(sql, sizeof sql,
			 "INSERT INTO sessions (id, created_at, updated_at, started_at, ended_at, end_reason, "
			 "title, canvas_uuids) VALUES ('s%d', 1, 1, %d, %d, 'ended', 't%d', '[]')",
			 i, i * 1000, i * 1000 + 500, i);
		assert_true(db.Exec(sql));
	}
	History::SessionStore store;
	assert_true(store.Attach(path));
	const auto page = store.List(2, 1);
	assert_int_equal((int)page.size(), 2);
	assert_string_equal(page[0].id.c_str(), "s4");
	assert_string_equal(page[1].id.c_str(), "s3");
}

static void test_remove_takes_children_with_it(void **state)
{
	(void)state;
	const std::string path = TempDbPath("remove.db");
	History::Db db;
	assert_true(db.Open(path));
	assert_true(db.Exec("INSERT INTO sessions (id, created_at, updated_at, started_at, title, canvas_uuids) "
			    "VALUES ('s1', 1, 1, 1000, 'doomed', '[]')"));
	assert_true(db.Exec("INSERT INTO session_health (session_id, t, bitrate_kbps, dropped_frames, "
			    "congestion_pct, encode_skipped, cpu_pct) VALUES ('s1', 2000, 1, 0, 0.0, 0, 0.0)"));

	History::SessionStore store;
	assert_true(store.Attach(path));
	assert_true(store.Remove("s1"));
	assert_int_equal((int)store.List(10, 0).size(), 0);
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM session_health"), 0);
}

// The recorder takes its clock and its samples as inputs so the whole write
// path is testable without a broadcast, a CEF message loop, or a real second
// passing.
static void test_recorder_opens_and_closes_a_session(void **state)
{
	(void)state;
	const std::string path = TempDbPath("recorder_basic.db");
	History::Db db;
	assert_true(db.Open(path));

	History::SessionRecorder rec;
	assert_true(rec.Attach(path));

	History::SessionStart start;
	start.title = "Tuesday";
	start.canvasUuids = {"cv-1"};
	start.startedAtMs = 1'000'000;
	History::DestinationRecord dest;
	dest.bindingUuid = "b1";
	dest.profileId = "p1";
	dest.platform = "youtube";
	dest.accountLabel = "Main";
	dest.title = "Sent title";
	start.destinations.push_back(dest);

	const std::string id = rec.Begin(start);
	assert_false(id.empty());
	assert_true(rec.IsRecording());

	History::SessionStore store;
	assert_true(store.Attach(path));
	// While running, ended_at is null. That is not a placeholder -- it is the
	// crash marker, and it must be true on disk during the broadcast.
	assert_false(store.Get(id)->endedAt.has_value());

	rec.End(1'003'600, "ended");
	assert_false(rec.IsRecording());

	const auto after = store.Get(id);
	assert_true(after->endedAt.has_value());
	assert_int_equal((int)*after->endedAt, 1'003'600);
	assert_string_equal(after->endReason.value().c_str(), "ended");
	assert_string_equal(store.DestinationsFor(id)[0].title.c_str(), "Sent title");
}

static void test_recorder_downsamples_to_ten_seconds(void **state)
{
	(void)state;
	const std::string path = TempDbPath("recorder_downsample.db");
	History::Db db;
	assert_true(db.Open(path));
	History::SessionRecorder rec;
	assert_true(rec.Attach(path));

	History::SessionStart start;
	start.startedAtMs = 0;
	const std::string id = rec.Begin(start);

	// 25 one-second ticks. At a 10s interval that is samples at t=0, 10s, 20s.
	for (int i = 0; i < 25; i++) {
		History::HealthSample s;
		s.tMs = (int64_t)i * 1000;
		s.bitrateKbps = 6000;
		s.cumulativeDroppedFrames = i;
		s.cumulativeEncodeSkipped = 0;
		s.congestionPct = 0.0;
		s.cpuPct = 12.0;
		rec.OnSample(s);
	}
	rec.End(25'000, "ended");

	History::SessionStore store;
	assert_true(store.Attach(path));
	const auto health = store.HealthFor(id);
	assert_int_equal((int)health.size(), 3);
	assert_int_equal((int)health[0].t, 0);
	assert_int_equal((int)health[1].t, 10'000);
	assert_int_equal((int)health[2].t, 20'000);
	// Stored as the delta over the interval, not the cumulative total: ten
	// ticks of one dropped frame each is ten, not the running sum.
	assert_int_equal((int)health[1].droppedFrames, 10);
}

static void test_recorder_survives_a_counter_reset(void **state)
{
	(void)state;
	const std::string path = TempDbPath("recorder_reset.db");
	History::Db db;
	assert_true(db.Open(path));
	History::SessionRecorder rec;
	assert_true(rec.Attach(path));
	History::SessionStart start;
	start.startedAtMs = 0;
	const std::string id = rec.Begin(start);

	History::HealthSample a;
	a.tMs = 0;
	a.cumulativeDroppedFrames = 100;
	rec.OnSample(a);
	// The user hit stats.reset: the counter goes backwards. A naive
	// subtraction would store a negative delta.
	History::HealthSample b;
	b.tMs = 10'000;
	b.cumulativeDroppedFrames = 3;
	rec.OnSample(b);
	rec.End(20'000, "ended");

	History::SessionStore store;
	assert_true(store.Attach(path));
	const auto health = store.HealthFor(id);
	assert_int_equal((int)health.back().droppedFrames, 3);
}

static void test_recorder_ignores_samples_when_not_recording(void **state)
{
	(void)state;
	const std::string path = TempDbPath("recorder_idle.db");
	History::Db db;
	assert_true(db.Open(path));
	History::SessionRecorder rec;
	assert_true(rec.Attach(path));
	History::HealthSample s;
	s.tMs = 1000;
	rec.OnSample(s); // no Begin() -- must be a no-op, not a crash
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM session_health"), 0);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_open_creates_database),
		cmocka_unit_test(test_open_sets_wal_and_version),
		cmocka_unit_test(test_migration_is_idempotent),
		cmocka_unit_test(test_newer_database_is_refused),
		cmocka_unit_test(test_updated_at_trigger_fires),
		cmocka_unit_test(test_delete_cascades_to_children),
		cmocka_unit_test(test_orm_schema_matches_migrated_schema),
		cmocka_unit_test(test_recovery_marks_unended_session_crashed),
		cmocka_unit_test(test_recovery_without_health_falls_back_to_start),
		cmocka_unit_test(test_recovery_leaves_clean_sessions_alone),
		cmocka_unit_test(test_list_is_newest_first_and_paged),
		cmocka_unit_test(test_remove_takes_children_with_it),
		cmocka_unit_test(test_recorder_opens_and_closes_a_session),
		cmocka_unit_test(test_recorder_downsamples_to_ten_seconds),
		cmocka_unit_test(test_recorder_survives_a_counter_reset),
		cmocka_unit_test(test_recorder_ignores_samples_when_not_recording),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
