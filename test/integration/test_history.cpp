#include "harness.hpp"

#include "history/Db.hpp"
#include "history/ScheduleRunner.hpp"
#include "history/ScheduleStore.hpp"
#include "history/Schema.hpp"
#include "history/SessionRecorder.hpp"
#include "history/SessionStore.hpp"

#include <string>
#include <vector>

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

// Winds a current database back to v1 by removing exactly what v2 added, rather
// than by pasting v1's DDL into this file. A copied fixture is a second
// description of the schema that nothing keeps honest -- it would keep passing
// against a v1 that had drifted out from under it.
static bool WindBackToV1(History::Db &db)
{
	return db.Exec("DROP TRIGGER trg_schedule_destinations_updated_at;"
		       "DROP TRIGGER trg_schedule_updated_at;"
		       "DROP TABLE schedule_destinations;"
		       "DROP TABLE schedule;"
		       "DROP INDEX idx_sessions_schedule;"
		       "PRAGMA user_version = 1");
}

// The upgrade an existing install actually performs. What matters is not that
// the new tables appear -- a fresh database proves that -- but that the rows
// already there survive it.
static void test_v1_upgrades_to_v2_preserving_rows(void **state)
{
	(void)state;
	const std::string path = TempDbPath("upgrade_v1_v2.db");
	{
		History::Db db;
		assert_true(db.Open(path));
		assert_true(SeedSession(db));
		assert_true(SeedDestination(db));
		assert_true(WindBackToV1(db));
		assert_int_equal(db.Version(), 1);
		db.Close();
	}
	History::Db db;
	assert_true(db.Open(path));
	assert_int_equal(db.Version(), History::kCurrentSchemaVersion);
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM sessions"), 1);
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM session_destinations"), 1);
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM schedule"), 0);
	db.Close();
}

// The runner drives `state` from several places, so it is constrained in the
// migration rather than in whichever write path happens to be calling.
static void test_schedule_state_is_constrained(void **state)
{
	(void)state;
	History::Db db;
	assert_true(db.Open(TempDbPath("schedule_state.db")));
	assert_true(db.Exec("INSERT INTO schedule (id, created_at, updated_at, starts_at, state) "
			    "VALUES ('e1', 1, 1, 1000, 'planned')"));
	assert_false(db.Exec("INSERT INTO schedule (id, created_at, updated_at, starts_at, state) "
			     "VALUES ('e2', 1, 1, 1000, 'nonsense')"));
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM schedule"), 1);
	db.Close();
}

static void test_schedule_delete_cascades_to_destinations(void **state)
{
	(void)state;
	History::Db db;
	assert_true(db.Open(TempDbPath("schedule_cascade.db")));
	assert_true(db.Exec("INSERT INTO schedule (id, created_at, updated_at, starts_at) VALUES ('e1', 1, 1, 1000)"));
	assert_true(db.Exec("INSERT INTO schedule_destinations (id, created_at, updated_at, schedule_id, profile_id) "
			    "VALUES ('sd1', 1, 1, 'e1', 'p1')"));
	assert_true(db.Exec("DELETE FROM schedule WHERE id = 'e1'"));
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM schedule_destinations"), 0);
	db.Close();
}

// Opens a migrated database and attaches a store over it, the way bootstrap
// does. Returned by value so each test gets its own file.
static History::ScheduleEntry MakeEntry(int64_t startsAt, const char *title)
{
	History::ScheduleEntry e;
	e.startsAt = startsAt;
	e.title = title;
	e.durationMin = 120;
	return e;
}

static void test_schedule_create_round_trips(void **state)
{
	(void)state;
	const std::string path = TempDbPath("schedule_create.db");
	History::Db db;
	assert_true(db.Open(path));
	db.Close();

	History::ScheduleStore store;
	assert_true(store.Attach(path));

	History::ScheduleEntry e = MakeEntry(5000, "Friday Night");
	History::ScheduleDestination d;
	d.profileId = "p1";
	d.title = "as sent";
	assert_true(store.Create(e, {d}));
	assert_false(e.id.empty());

	const auto got = store.Get(e.id);
	assert_non_null(got.get());
	assert_string_equal(got->title.c_str(), "Friday Night");
	assert_string_equal(got->state.c_str(), History::ScheduleState::kPlanned);
	assert_int_equal((int)store.DestinationsFor(e.id).size(), 1);
}

// Destinations are replaced wholesale, so the count after an update is the new
// set's -- not the union with what was there before.
static void test_schedule_update_replaces_destinations(void **state)
{
	(void)state;
	const std::string path = TempDbPath("schedule_update.db");
	History::Db db;
	assert_true(db.Open(path));
	db.Close();

	History::ScheduleStore store;
	assert_true(store.Attach(path));

	History::ScheduleEntry e = MakeEntry(5000, "before");
	History::ScheduleDestination a;
	a.profileId = "p1";
	History::ScheduleDestination b;
	b.profileId = "p2";
	assert_true(store.Create(e, {a, b}));
	assert_int_equal((int)store.DestinationsFor(e.id).size(), 2);

	e.title = "after";
	History::ScheduleDestination only;
	only.profileId = "p3";
	assert_true(store.Update(e, {only}));

	const auto got = store.Get(e.id);
	assert_non_null(got.get());
	assert_string_equal(got->title.c_str(), "after");
	const auto dests = store.DestinationsFor(e.id);
	assert_int_equal((int)dests.size(), 1);
	assert_string_equal(dests[0].profileId.c_str(), "p3");
}

// Half-open [from, to): an entry exactly on `to` belongs to the next view, or
// adjacent months both claim it.
static void test_schedule_range_is_half_open_and_ordered(void **state)
{
	(void)state;
	const std::string path = TempDbPath("schedule_range.db");
	History::Db db;
	assert_true(db.Open(path));
	db.Close();

	History::ScheduleStore store;
	assert_true(store.Attach(path));

	History::ScheduleEntry late = MakeEntry(3000, "late");
	History::ScheduleEntry early = MakeEntry(1000, "early");
	History::ScheduleEntry edge = MakeEntry(4000, "on the boundary");
	assert_true(store.Create(late, {}));
	assert_true(store.Create(early, {}));
	assert_true(store.Create(edge, {}));

	const auto rows = store.ListRange(1000, 4000);
	assert_int_equal((int)rows.size(), 2);
	assert_string_equal(rows[0].entry.title.c_str(), "early");
	assert_string_equal(rows[1].entry.title.c_str(), "late");
}

// Deleting a plan must not delete the broadcast it planned. The session keeps
// its row and loses only the link.
static void test_schedule_remove_keeps_the_session_it_planned(void **state)
{
	(void)state;
	const std::string path = TempDbPath("schedule_remove.db");
	History::Db db;
	assert_true(db.Open(path));
	assert_true(SeedSession(db));

	History::ScheduleStore store;
	assert_true(store.Attach(path));
	History::ScheduleEntry e = MakeEntry(5000, "planned");
	History::ScheduleDestination d;
	d.profileId = "p1";
	assert_true(store.Create(e, {d}));
	assert_true(db.Exec(("UPDATE sessions SET schedule_id = '" + e.id + "' WHERE id = 's1'").c_str()));

	assert_true(store.Remove(e.id));
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM schedule"), 0);
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM schedule_destinations"), 0);
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM sessions"), 1);
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM sessions WHERE schedule_id IS NULL"), 1);
	db.Close();
}

// Only planned and armed can be missed. A settled entry re-marked every tick
// would rewrite its own history.
static void test_schedule_sweep_marks_only_unsettled_entries(void **state)
{
	(void)state;
	const std::string path = TempDbPath("schedule_sweep.db");
	History::Db db;
	assert_true(db.Open(path));
	db.Close();

	History::ScheduleStore store;
	assert_true(store.Attach(path));

	History::ScheduleEntry planned = MakeEntry(1000, "planned");
	History::ScheduleEntry armed = MakeEntry(1000, "armed");
	History::ScheduleEntry done = MakeEntry(1000, "done");
	History::ScheduleEntry future = MakeEntry(9000, "future");
	assert_true(store.Create(planned, {}));
	assert_true(store.Create(armed, {}));
	assert_true(store.Create(done, {}));
	assert_true(store.Create(future, {}));
	assert_true(store.SetState(armed.id, History::ScheduleState::kArmed));
	assert_true(store.SetState(done.id, History::ScheduleState::kDone));

	assert_int_equal(store.SweepMissed(5000), 2);

	assert_string_equal(store.Get(planned.id)->state.c_str(), History::ScheduleState::kMissed);
	assert_string_equal(store.Get(armed.id)->state.c_str(), History::ScheduleState::kMissed);
	assert_string_equal(store.Get(done.id)->state.c_str(), History::ScheduleState::kDone);
	assert_string_equal(store.Get(future.id)->state.c_str(), History::ScheduleState::kPlanned);

	// Idempotent: a second sweep finds nothing left to settle.
	assert_int_equal(store.SweepMissed(5000), 0);
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

// A runner over its own temp database with every outside effect captured, so the
// state machine is driven entirely by moving `clock` -- no waiting, no broadcast,
// and nothing of the user's touched.
struct RunnerFixture {
	History::Db db;
	History::ScheduleStore store;
	History::ScheduleRunner runner;
	int64_t clock = 0;
	int goLiveCalls = 0;
	int changedCalls = 0;
	std::vector<std::string> logLines;
};

static void OpenRunner(RunnerFixture &f, const char *name)
{
	const std::string path = TempDbPath(name);
	assert_true(f.db.Open(path));
	assert_true(f.store.Attach(path));
	f.runner.Attach(&f.store);
	f.runner.nowMs = [&f] {
		return f.clock;
	};
	f.runner.goLive = [&f] {
		f.goLiveCalls++;
	};
	f.runner.onChanged = [&f] {
		f.changedCalls++;
	};
	f.runner.log = [&f](const std::string &line) {
		f.logLines.push_back(line);
	};
}

static std::string SeedEntry(RunnerFixture &f, int64_t startsAt, bool autoStart, bool withDestination = true)
{
	History::ScheduleEntry entry = MakeEntry(startsAt, "Friday Night");
	entry.autoStart = autoStart ? 1 : 0;
	std::vector<History::ScheduleDestination> destinations;
	if (withDestination) {
		History::ScheduleDestination d;
		d.profileId = "p1";
		destinations.push_back(d);
	}
	assert_true(f.store.Create(entry, destinations));
	return entry.id;
}

static bool LoggedLike(const RunnerFixture &f, const char *fragment)
{
	for (const std::string &line : f.logLines) {
		if (line.find(fragment) != std::string::npos) {
			return true;
		}
	}
	return false;
}

static void test_runner_arms_at_the_five_minute_lead(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_arm.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt - History::kArmLeadMs - 1000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kPlanned);
	// Nothing happened, so nothing was pushed. The event is per transition, not
	// per tick -- a 1 Hz emit would repaint the calendar forever.
	assert_int_equal(f.changedCalls, 0);

	f.clock = startsAt - History::kArmLeadMs;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_int_equal(f.changedCalls, 1);

	// The entry an armed occurrence belongs to is what stamps schedule_id onto the
	// session row, whether the broadcast is started by the runner or by hand.
	assert_string_equal(f.runner.ActiveEntryId().c_str(), id.c_str());

	f.runner.Tick();
	assert_int_equal(f.changedCalls, 1);
}

static void test_runner_opens_the_countdown_at_sixty_seconds(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_countdown.db");
	const int64_t startsAt = 10'000'000;
	SeedEntry(f, startsAt, true);

	f.clock = startsAt - History::kCountdownLeadMs - 1000;
	f.runner.Tick();
	assert_false(LoggedLike(f, "countdown open"));

	f.clock = startsAt - History::kCountdownLeadMs;
	f.runner.Tick();
	assert_true(LoggedLike(f, "countdown open"));
}

static void test_runner_cancel_disarms_only_this_occurrence(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_cancel.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 30'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);

	std::string error;
	assert_true(f.runner.CancelCountdown(id, error));

	// Back to planned and flagged, which is how the UI tells a cancelled
	// occurrence from one that was never armed -- both read `planned`.
	const auto after = f.store.Get(id);
	assert_string_equal(after->state.c_str(), History::ScheduleState::kPlanned);
	assert_true(f.runner.IsCountdownCanceled(id));
	// The row itself is untouched: cancelling an occurrence is not deleting a plan.
	assert_string_equal(after->title.c_str(), "Friday Night");
	assert_true(after->startsAt == startsAt);
	assert_int_equal((int)f.store.DestinationsFor(id).size(), 1);

	// It must neither re-arm nor auto-start for the rest of this occurrence.
	f.clock = startsAt - 10'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kPlanned);
	f.clock = startsAt + 1000;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 0);

	// It settles as missed like any other unstarted entry, and the flag outlives
	// that so the chip can still say the start was cancelled, not merely skipped.
	f.clock = startsAt + History::kMissedGraceMs + 1000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kMissed);
	assert_true(f.runner.IsCountdownCanceled(id));
}

static void test_runner_cancel_refuses_an_entry_that_is_not_armed(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_cancel_refuse.db");
	const std::string id = SeedEntry(f, 10'000'000, true);

	std::string error;
	assert_false(f.runner.CancelCountdown(id, error));
	assert_false(error.empty());
	assert_false(f.runner.IsCountdownCanceled(id));

	assert_false(f.runner.CancelCountdown("no-such-entry", error));
	assert_false(error.empty());
}

static void test_runner_never_starts_an_entry_with_auto_start_off(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_manual.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);

	// Past T-0 it waits for the user rather than going live on its own.
	f.clock = startsAt + 1000;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 0);
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);

	f.clock = startsAt + History::kMissedGraceMs + 1000;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 0);
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kMissed);
}

static void test_runner_auto_starts_once_at_zero(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_autostart.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 0);

	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 1);
	// Requesting a start is not a broadcast: the row moves to `live` only once the
	// outputs report it, so a start that never comes up cannot read as one that did.
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);

	f.clock = startsAt + 1000;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 1);

	f.runner.NoteWentLive();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kLive);

	// A live entry is settled: the missed sweep must not rewrite it.
	f.clock = startsAt + History::kMissedGraceMs + 1000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kLive);

	f.runner.NoteStoppedStreaming();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kDone);
}

static void test_runner_marks_a_passed_entry_missed(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_missed.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt + History::kMissedGraceMs + 1;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kMissed);
	assert_int_equal(f.changedCalls, 1);
}

// Design 6: broadcasting to nowhere is worse than not broadcasting. The refusal
// has to say why, or the entry just stops happening for no stated reason.
static void test_runner_refuses_auto_start_with_no_armable_destination(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_blocked.db");
	f.runner.canArm = [](const std::string &, std::string &reason) {
		reason = "'Main' is not connected";
		return false;
	};
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	// Surfaced at arm, five minutes before it matters, so there is time to fix it.
	f.clock = startsAt - History::kArmLeadMs;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_string_equal(f.runner.BlockReason(id).c_str(), "'Main' is not connected");

	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 0);

	// It settles as missed, but the reason survives so the chip can explain it
	// rather than showing a start that silently did not happen.
	f.clock = startsAt + History::kMissedGraceMs + 1000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kMissed);
	assert_string_equal(f.runner.BlockReason(id).c_str(), "'Main' is not connected");
}

static void test_runner_refuses_auto_start_with_no_destinations(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_no_dests.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true, false);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_false(f.runner.BlockReason(id).empty());

	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 0);
}

// A destination that comes back inside the grace window still gets to go live --
// the refusal is re-evaluated every tick rather than latched at T-0.
static void test_runner_starts_once_a_destination_recovers(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_recovers.db");
	bool connected = false;
	f.runner.canArm = [&connected](const std::string &, std::string &reason) {
		reason = "'Main' is not connected";
		return connected;
	};
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 0);

	connected = true;
	f.clock = startsAt + 15'000;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 1);
	assert_true(f.runner.BlockReason(id).empty());
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_open_creates_database),
		cmocka_unit_test(test_open_sets_wal_and_version),
		cmocka_unit_test(test_migration_is_idempotent),
		cmocka_unit_test(test_v1_upgrades_to_v2_preserving_rows),
		cmocka_unit_test(test_schedule_state_is_constrained),
		cmocka_unit_test(test_schedule_delete_cascades_to_destinations),
		cmocka_unit_test(test_schedule_create_round_trips),
		cmocka_unit_test(test_schedule_update_replaces_destinations),
		cmocka_unit_test(test_schedule_range_is_half_open_and_ordered),
		cmocka_unit_test(test_schedule_remove_keeps_the_session_it_planned),
		cmocka_unit_test(test_schedule_sweep_marks_only_unsettled_entries),
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
		cmocka_unit_test(test_runner_arms_at_the_five_minute_lead),
		cmocka_unit_test(test_runner_opens_the_countdown_at_sixty_seconds),
		cmocka_unit_test(test_runner_cancel_disarms_only_this_occurrence),
		cmocka_unit_test(test_runner_cancel_refuses_an_entry_that_is_not_armed),
		cmocka_unit_test(test_runner_never_starts_an_entry_with_auto_start_off),
		cmocka_unit_test(test_runner_auto_starts_once_at_zero),
		cmocka_unit_test(test_runner_marks_a_passed_entry_missed),
		cmocka_unit_test(test_runner_refuses_auto_start_with_no_armable_destination),
		cmocka_unit_test(test_runner_refuses_auto_start_with_no_destinations),
		cmocka_unit_test(test_runner_starts_once_a_destination_recovers),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
