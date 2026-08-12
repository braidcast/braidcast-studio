#include "harness.hpp"

#include "history/Db.hpp"
#include "history/ScheduleRunner.hpp"
#include "history/ScheduleStore.hpp"
#include "history/ScheduledSetup.hpp"
#include "history/Schema.hpp"
#include "history/SessionRecorder.hpp"
#include "history/SessionStore.hpp"

#include <map>
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

// Winds a current database back a version by removing exactly what that version
// added, rather than by pasting the older DDL into this file. A copied fixture is
// a second description of the schema that nothing keeps honest -- it would keep
// passing against an old version that had drifted out from under it. WindBackToV1
// needs no help from WindBackToV2: v3's column lives on a table v2 created, and
// dropping that table takes the column with it.
static bool WindBackToV1(History::Db &db)
{
	return db.Exec("DROP TRIGGER trg_schedule_destinations_updated_at;"
		       "DROP TRIGGER trg_schedule_updated_at;"
		       "DROP TABLE schedule_destinations;"
		       "DROP TABLE schedule;"
		       "DROP INDEX idx_sessions_schedule;"
		       "PRAGMA user_version = 1");
}

static bool WindBackToV2(History::Db &db)
{
	return db.Exec("ALTER TABLE schedule_destinations DROP COLUMN category_id;"
		       "PRAGMA user_version = 2");
}

// The upgrade the oldest install actually performs, every version at once. What
// matters is not that the new tables appear -- a fresh database proves that --
// but that the rows already there survive it.
static void test_v1_upgrades_to_current_preserving_rows(void **state)
{
	(void)state;
	const std::string path = TempDbPath("upgrade_from_v1.db");
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

// v2 shipped before the category split, so an existing install arrives here with
// entries already in it. What matters is that they survive the added column, not
// that a fresh database has one.
static void test_v2_upgrades_to_v3_preserving_rows(void **state)
{
	(void)state;
	const std::string path = TempDbPath("upgrade_v2_v3.db");
	{
		History::Db db;
		assert_true(db.Open(path));
		assert_true(db.Exec("INSERT INTO schedule (id, created_at, updated_at, starts_at, title) "
				    "VALUES ('e1', 1, 1, 1000, 'Friday')"));
		assert_true(db.Exec("INSERT INTO schedule_destinations "
				    "(id, created_at, updated_at, schedule_id, profile_id, category) "
				    "VALUES ('sd1', 1, 1, 'e1', 'p1', 'Just Chatting')"));
		assert_true(WindBackToV2(db));
		assert_int_equal(db.Version(), 2);
		// The column really is gone, or the upgrade below would prove nothing.
		assert_false(db.Exec("SELECT category_id FROM schedule_destinations"));
		db.Close();
	}
	History::Db db;
	assert_true(db.Open(path));
	assert_int_equal(db.Version(), History::kCurrentSchemaVersion);
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM schedule"), 1);
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM schedule_destinations"), 1);
	// A v2 row's category was always a display name, so it stays one and the new
	// column starts empty rather than guessing an id out of it.
	assert_int_equal((int)db.ScalarInt("SELECT COUNT(*) FROM schedule_destinations "
					   "WHERE category = 'Just Chatting' AND category_id = ''"),
			 1);
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
	d.category = "Just Chatting";
	d.categoryId = "509658";
	assert_true(store.Create(e, {d}));
	assert_false(e.id.empty());

	const auto got = store.Get(e.id);
	assert_non_null(got.get());
	assert_string_equal(got->title.c_str(), "Friday Night");
	assert_string_equal(got->state.c_str(), History::ScheduleState::kPlanned);

	// Both halves of the category survive the round trip. The id is the one a
	// go-live sends, so losing it would leave the arm applying a name no provider
	// can act on.
	const auto dests = store.DestinationsFor(e.id);
	assert_int_equal((int)dests.size(), 1);
	assert_string_equal(dests[0].category.c_str(), "Just Chatting");
	assert_string_equal(dests[0].categoryId.c_str(), "509658");
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
	// Every call the runner made to the injected apply, and the subset of those that
	// succeeded. A refused apply still counts as an attempt, so "the runner never even
	// asked" stays provable against a fixture whose apply is refusing everything.
	int applyAttempts = 0;
	int applyCalls = 0;
	int revertCalls = 0;
	// What the injected apply answers, and what a broadcast the runner did not
	// start looks like from the outside.
	bool applyOk = true;
	std::string applyReason = "the routing could not be loaded";
	bool streaming = false;
	// The user's "every destination or nothing" setting, injected like the rest of the
	// runner's outside world.
	bool requireAllDestinations = false;
	std::vector<std::string> appliedProfiles;
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
	f.runner.isStreaming = [&f] {
		return f.streaming;
	};
	f.runner.requireAllDestinations = [&f] {
		return f.requireAllDestinations;
	};
	f.runner.applyEntry = [&f](const std::vector<History::ScheduleDestination> &destinations, std::string &reason) {
		f.applyAttempts++;
		if (!f.applyOk) {
			reason = f.applyReason;
			return false;
		}
		f.applyCalls++;
		f.appliedProfiles.clear();
		for (const History::ScheduleDestination &d : destinations) {
			f.appliedProfiles.push_back(d.profileId);
		}
		return true;
	};
	f.runner.revertEntry = [&f] {
		f.revertCalls++;
		f.appliedProfiles.clear();
	};
	f.runner.log = [&f](const std::string &line) {
		f.logLines.push_back(line);
	};
}

static std::string SeedEntryWith(RunnerFixture &f, int64_t startsAt, bool autoStart,
				 const std::vector<std::string> &profileIds)
{
	History::ScheduleEntry entry = MakeEntry(startsAt, "Friday Night");
	entry.autoStart = autoStart ? 1 : 0;
	std::vector<History::ScheduleDestination> destinations;
	for (const std::string &profileId : profileIds) {
		History::ScheduleDestination d;
		d.profileId = profileId;
		destinations.push_back(d);
	}
	assert_true(f.store.Create(entry, destinations));
	return entry.id;
}

static std::string SeedEntry(RunnerFixture &f, int64_t startsAt, bool autoStart, bool withDestination = true)
{
	return SeedEntryWith(f, startsAt, autoStart,
			     withDestination ? std::vector<std::string>{"p1"} : std::vector<std::string>{});
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

	// Merely armed is not yet claimed: nothing has asked to start this occurrence,
	// so it is not what a broadcast starting right now would belong to. Only the
	// auto-start, StartNow, or AdoptImminentArmed turn an armed entry into one.
	assert_string_equal(f.runner.ActiveEntryId().c_str(), "");

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
	// Nothing to put back: arming never touched the routing, so a cancelled
	// countdown leaves the user's configuration exactly as they left it.
	assert_int_equal(f.applyAttempts, 0);
	assert_int_equal(f.revertCalls, 0);

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

// Design 5 step 2: the entry's destinations and metadata become the live
// configuration, or an auto-start broadcasts to whatever happened to be enabled
// under the previous stream's title. It happens at T-0 and not at the arm: this
// rewrites configuration the user owns, so the window in which that is true is one
// go-live rather than five minutes.
static void test_runner_applies_at_zero_not_at_arm(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_applied_live.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - History::kArmLeadMs;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_int_equal(f.applyAttempts, 0);

	// Still nothing through the countdown, a minute from the start.
	f.clock = startsAt - History::kCountdownLeadMs;
	f.runner.Tick();
	assert_int_equal(f.applyAttempts, 0);

	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal(f.goLiveCalls, 1);
	assert_int_equal((int)f.appliedProfiles.size(), 1);
	assert_string_equal(f.appliedProfiles[0].c_str(), "p1");

	f.streaming = true;
	f.runner.NoteWentLive();

	// Well past the point an unstarted entry would have settled. A live one keeps
	// its routing, or the broadcast would lose its destinations mid-stream.
	f.clock = startsAt + History::kMissedGraceMs + 60'000;
	f.runner.Tick();
	assert_int_equal(f.revertCalls, 0);
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kLive);

	f.streaming = false;
	f.runner.NoteStoppedStreaming();
	assert_int_equal(f.revertCalls, 1);
}

// The request went out and the outputs never came up. The row settling and the
// start being over are two different things: the row settles on the missed grace so
// the calendar stops calling the entry upcoming, while the configuration is held
// until the start is given up on -- a prelude working through platform round trips
// and RTMP retries past the grace must not have its routing pulled out from under
// it. Once it is given up on, the configuration goes back, or a failed start would
// leave the user's destinations silently rewritten.
static void test_runner_holds_the_routing_until_a_start_is_given_up_on(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_applied_missed.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal(f.goLiveCalls, 1);

	// Past the missed grace, and neither the row nor the routing moves: the sweep
	// skips a start that is on its way up, or the broadcast it is about to bring up
	// would be filed as missed.
	f.clock = startsAt + History::kMissedGraceMs + 1000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_int_equal(f.revertCalls, 0);
	assert_true(f.runner.IsStartInFlight(id));

	// Still held most of the way to the deadline.
	f.clock = startsAt + History::kStartInFlightMs - 1000;
	f.runner.Tick();
	assert_int_equal(f.revertCalls, 0);

	// Given up on: the exclusion lifts, so the same tick settles the row and puts
	// the configuration back.
	f.clock = startsAt + History::kStartInFlightMs + 1000;
	f.runner.Tick();
	assert_false(f.runner.IsStartInFlight(id));
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kMissed);
	assert_int_equal(f.revertCalls, 1);
}

// A prelude that runs past the missed grace and then succeeds. The row has to still
// be `armed` when the outputs report, or NoteWentLive cannot move it to `live`,
// NoteStoppedStreaming never reaches `done`, and ActiveEntryId stamps no
// schedule_id -- a live broadcast filed as missed, with the session it produced
// orphaned from the plan it fulfilled.
static void test_runner_links_a_broadcast_that_came_up_after_the_grace(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_slow_prelude.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 1);

	f.clock = startsAt + History::kMissedGraceMs + 30'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	// What stamps schedule_id onto the session row.
	assert_string_equal(f.runner.ActiveEntryId().c_str(), id.c_str());

	f.streaming = true;
	f.runner.NoteWentLive();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kLive);
	assert_string_equal(f.runner.ActiveEntryId().c_str(), id.c_str());

	f.clock = startsAt + History::kMissedGraceMs + 60'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kLive);

	f.streaming = false;
	f.runner.NoteStoppedStreaming();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kDone);
	assert_int_equal(f.revertCalls, 1);
}

// The go-live was refused outright -- a destination that could not be prepared. No
// output comes up, so no stop edge will ever fire, and this refusal is the only
// word the runner gets. Untold, the entry's destinations stay enabled and a manual
// go-live in that window streams to them instead of the user's own.
static void test_runner_puts_the_routing_back_when_the_go_live_is_refused(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_start_refused.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);
	const std::string other = SeedEntry(f, startsAt + 60'000, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal(f.goLiveCalls, 1);
	assert_true(f.runner.IsStartInFlight(id));

	f.runner.NoteStartFailed();
	assert_int_equal(f.revertCalls, 1);
	assert_false(f.runner.IsStartInFlight(id));

	// Everything the commitment was blocking is released. The next entry gets its
	// own start instead of being refused for ten minutes because of a broadcast that
	// never happened.
	f.clock = startsAt + 60'000;
	f.runner.Tick();
	assert_true(f.runner.BlockReason(other).empty());
	assert_int_equal(f.goLiveCalls, 2);
	assert_int_equal(f.applyCalls, 2);

	// And the refused entry settles on the ordinary grace rather than being held
	// armed for the whole in-flight allowance.
	f.clock = startsAt + History::kMissedGraceMs + 1000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kMissed);
}

// An entry the user started by hand during its armed window. AdoptImminentArmed is
// what makes it ask -- exactly what StartStreamingAllAdoptingSchedule calls before
// the go-live it wraps -- so the broadcast is still this entry's, and deleting it
// mid-stream nulls the running session's schedule_id.
static void test_runner_guards_a_manually_started_entry(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_manual_live.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_false(f.runner.IsStartInFlight(id));

	f.runner.AdoptImminentArmed();
	f.streaming = true;
	f.runner.NoteWentLive();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kLive);
	assert_true(f.runner.IsStartInFlight(id));

	f.streaming = false;
	f.runner.NoteStoppedStreaming();
	assert_false(f.runner.IsStartInFlight(id));
}

// The same window closes the two refusals that key off it, so an entry nothing is
// doing anything with stops refusing to be cancelled or deleted.
static void test_runner_frees_a_given_up_start_for_deletion(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_settled_request.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	assert_true(f.runner.IsStartInFlight(id));

	f.clock = startsAt + History::kStartInFlightMs + 1000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kMissed);
	assert_false(f.runner.IsStartInFlight(id));

	// A live entry keeps it for as long as it runs, whatever the deadline says:
	// deleting one mid-broadcast is the case the refusal exists for.
	const std::string other = SeedEntry(f, f.clock + 60'000, true);
	f.runner.Tick();
	f.clock += 60'000;
	f.runner.Tick();
	f.streaming = true;
	f.runner.NoteWentLive();
	assert_string_equal(f.store.Get(other)->state.c_str(), History::ScheduleState::kLive);
	f.clock += History::kStartInFlightMs + 1000;
	f.runner.Tick();
	assert_true(f.runner.IsStartInFlight(other));
}

// Two entries whose starts fall inside one prelude. The first one's request owns
// the routing until it resolves, so the second is refused rather than taking it --
// the outputs have not reported yet, so nothing reads as streaming.
static void test_runner_refuses_a_start_while_another_is_on_its_way_up(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_overlapping_starts.db");
	const int64_t startsAt = 10'000'000;
	const std::string first = SeedEntry(f, startsAt, true);
	const std::string second = SeedEntry(f, startsAt + 30'000, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal(f.goLiveCalls, 1);

	// Nothing is streaming -- the first start has not reported -- so only the
	// in-flight request can refuse this one.
	f.clock = startsAt + 30'000;
	f.runner.Tick();
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal(f.goLiveCalls, 1);
	assert_int_equal(f.revertCalls, 0);
	assert_string_equal(f.runner.BlockReason(second).c_str(), History::kAlreadyStreamingReason);
	assert_string_equal(f.runner.ActiveEntryId().c_str(), first.c_str());
}

// Never take the routing away from a broadcast that is already running. The
// refusal shows on the chip for the whole armed window, which is time enough to
// stop the other broadcast before the start is due.
static void test_runner_refuses_to_start_while_something_is_streaming(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_already_live.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);
	f.streaming = true;

	f.clock = startsAt - History::kArmLeadMs;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_string_equal(f.runner.BlockReason(id).c_str(), History::kAlreadyStreamingReason);

	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 0);
	assert_int_equal(f.applyAttempts, 0);

	// Stopped in time: the refusal clears and the entry still gets its start.
	f.streaming = false;
	f.clock = startsAt + 15'000;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 1);
	assert_int_equal(f.applyCalls, 1);
}

// The same refusal, but the routing is taken AFTER the entry armed clean. That is the
// only shape a go-live prelude can take -- one begins in the middle of an arm window
// and runs for seconds -- and it is the ordering the refusal has to survive: nothing is
// latched at arm time, so RefreshArmability running ahead of StartIfDue on the same
// tick is what puts the reason there in time.
//
// applyAttempts rather than applyCalls: the damage this prevents is the apply itself,
// which rewrites the routing a running go-live already snapshotted. "Never asked" and
// "asked and was refused" are the same applyCalls and very different outcomes.
static void test_runner_refuses_a_start_when_the_routing_is_taken_mid_window(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_taken_mid_window.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - History::kArmLeadMs;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_true(f.runner.BlockReason(id).empty());

	f.streaming = true;
	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyAttempts, 0);
	assert_int_equal(f.goLiveCalls, 0);
	assert_string_equal(f.runner.BlockReason(id).c_str(), History::kAlreadyStreamingReason);

	// The occurrence never asked, so nothing latched: it is still free to start once
	// the routing is its own again inside the grace window.
	f.streaming = false;
	f.clock = startsAt + 15'000;
	f.runner.Tick();
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal(f.goLiveCalls, 1);
}

// An apply that refuses is a refused start, not a start onto whatever was there.
// The reason is not latched: the next tick asks again, so a blocker that clears
// inside the grace window still gets its broadcast.
static void test_runner_refuses_a_start_the_apply_refused(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_apply_refused.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);
	f.applyOk = false;

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 0);
	// Refused by the apply itself rather than by a gate that never reached it.
	assert_int_equal(f.applyAttempts, 1);
	assert_int_equal(f.applyCalls, 0);
	assert_string_equal(f.runner.BlockReason(id).c_str(), "the routing could not be loaded");

	f.applyOk = true;
	f.clock = startsAt + 15'000;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 1);
	assert_true(f.runner.BlockReason(id).empty());
}

// The reason a refused apply leaves behind is something schedule.list reports, so the
// tick that refuses has to say the tick changed something -- otherwise the chip shows
// an entry whose start time passed and says nothing about why until something else
// happens to push. Nothing else on this tick reports a change: the arm already
// happened, the row does not move, and the armability pass finds nothing wrong with
// the entry, so the push can only have come from the refusal itself.
static void test_runner_pushes_the_change_when_an_apply_refusal_blocks_a_start(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_apply_refusal_pushes.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);
	f.applyOk = false;

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_int_equal(f.changedCalls, 1); // the arm
	assert_true(f.runner.BlockReason(id).empty());

	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyAttempts, 1);
	assert_int_equal(f.applyCalls, 0);
	assert_string_equal(f.runner.BlockReason(id).c_str(), "the routing could not be loaded");
	assert_int_equal(f.changedCalls, 2);
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
}

// Cancelling a countdown means "do not start", never "stop the broadcast". Once
// the request is out the outputs are on their way up, and putting the routing back
// underneath them would send that broadcast somewhere nobody asked for.
static void test_runner_refuses_cancel_once_the_start_is_requested(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_cancel_after_start.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 1);

	std::string error;
	assert_false(f.runner.CancelCountdown(id, error));
	assert_false(error.empty());
	assert_int_equal(f.revertCalls, 0);
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_false(f.runner.IsCountdownCanceled(id));
}

// Two entries whose windows overlap. Only one can hold the routing, and the first
// to start holds it: the second is refused for as long as that broadcast runs
// rather than taking it over halfway through.
static void test_runner_holds_the_routing_for_one_entry_at_a_time(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_overlapping.db");
	const int64_t startsAt = 10'000'000;
	const std::string first = SeedEntry(f, startsAt, true);
	const std::string second = SeedEntry(f, startsAt + 30'000, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(first)->state.c_str(), History::ScheduleState::kArmed);
	assert_string_equal(f.store.Get(second)->state.c_str(), History::ScheduleState::kArmed);

	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyCalls, 1);
	f.streaming = true;
	f.runner.NoteWentLive();
	assert_string_equal(f.store.Get(first)->state.c_str(), History::ScheduleState::kLive);

	f.clock = startsAt + 30'000;
	f.runner.Tick();
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal(f.goLiveCalls, 1);
	assert_int_equal(f.revertCalls, 0);
	assert_string_equal(f.runner.BlockReason(second).c_str(), History::kAlreadyStreamingReason);
	// The live entry keeps its own routing while the second is refused.
	assert_string_equal(f.runner.ActiveEntryId().c_str(), first.c_str());
}

// Deleting an entry the runner is holding. It stops stamping a dead id onto the
// next broadcast, but it must not put the routing back: the start is already
// requested and the outputs are coming up, which is exactly what CancelCountdown
// refuses to undo. Deleting cannot be a way around that refusal.
static void test_runner_lets_go_of_a_deleted_entry_without_reverting(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_deleted.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyCalls, 1);
	assert_true(f.runner.IsStartInFlight(id));
	assert_string_equal(f.runner.ActiveEntryId().c_str(), id.c_str());

	assert_true(f.store.Remove(id));
	f.runner.NoteEntryChanged(id);
	assert_string_equal(f.runner.ActiveEntryId().c_str(), "");
	assert_int_equal(f.revertCalls, 0);
	assert_false(f.runner.IsCountdownCanceled(id));

	// The broadcast's own stop edge is what puts it back, and the runner no longer
	// holds the entry, so it does not ask twice.
	f.runner.NoteStoppedStreaming();
	assert_int_equal(f.revertCalls, 0);
}

// Deleting one the runner armed but never started has nothing to hold on to: the
// apply happens at T-0, so an armed entry loaded no routing to put back.
static void test_runner_lets_go_of_a_deleted_armed_entry(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_deleted_armed.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_false(f.runner.IsStartInFlight(id));

	assert_true(f.store.Remove(id));
	f.runner.NoteEntryChanged(id);
	assert_string_equal(f.runner.ActiveEntryId().c_str(), "");
	// The runner let go: there is no longer an armed entry for a manual go-live to
	// adopt. Asked through the adopt path rather than ActiveEntryId, which answers
	// empty for a merely-armed entry either way and so could not tell the two apart.
	f.runner.AdoptImminentArmed();
	assert_int_equal(f.applyAttempts, 0);
	assert_int_equal(f.revertCalls, 0);
}

// Moved a day out on the calendar. Left armed, the chip reads armed until the new
// start and a manual go-live in between would adopt this entry's routing.
static void test_runner_disarms_an_entry_moved_out_of_its_window(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_rescheduled.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);

	History::ScheduleEntry moved = *f.store.Get(id);
	moved.startsAt = startsAt + 24 * 60 * 60 * 1000;
	History::ScheduleDestination d;
	d.profileId = "p1";
	assert_true(f.store.Update(moved, {d}));

	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kPlanned);
	// Disarmed for real: a manual go-live now finds nothing to adopt, which is the
	// difference the old ActiveEntryId assertion used to catch before a merely-armed
	// entry stopped being claimed on its own.
	f.runner.AdoptImminentArmed();
	assert_int_equal(f.applyAttempts, 0);
	// Arming loaded nothing, so a disarm has nothing to put back.
	assert_int_equal(f.revertCalls, 0);
}

// The bridge tells the runner as soon as the edit lands, so the disarm does not
// wait for the next tick to reach a UI that is repainting right now.
static void test_runner_disarms_on_an_edit_without_waiting_for_a_tick(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_edit_disarms.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);

	History::ScheduleEntry moved = *f.store.Get(id);
	moved.startsAt = startsAt + 24 * 60 * 60 * 1000;
	assert_true(f.store.Update(moved, {}));
	f.runner.NoteEntryChanged(id);
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kPlanned);
	assert_string_equal(f.runner.ActiveEntryId().c_str(), "");
}

// Armability is re-asked every tick while armed, not latched at the arm instant --
// otherwise a disconnect at T-2min stays invisible until T-0 and a reconnect leaves
// a refusal on the chip that stopped being true.
static void test_runner_tracks_armability_through_the_window(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_reason_refresh.db");
	bool connected = false;
	f.runner.canArm = [&connected](const std::string &, std::string &reason) {
		reason = "'Main' is not connected";
		return connected;
	};
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt - History::kArmLeadMs;
	f.runner.Tick();
	assert_string_equal(f.runner.BlockReason(id).c_str(), "'Main' is not connected");

	connected = true;
	f.clock = startsAt - 180'000;
	f.runner.Tick();
	assert_true(f.runner.BlockReason(id).empty());

	connected = false;
	f.clock = startsAt - 120'000;
	f.runner.Tick();
	assert_string_equal(f.runner.BlockReason(id).c_str(), "'Main' is not connected");
}

// A canArm that says yes to the first destination and no to the rest. The armability
// pass used to return on that first yes, so an entry naming three went live on one
// and reported nothing about the other two.
static void test_runner_asks_every_destination_not_just_the_first(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_every_destination.db");
	std::vector<std::string> asked;
	f.runner.canArm = [&asked](const std::string &profileId, std::string &reason) {
		asked.push_back(profileId);
		if (profileId == "p1") {
			return true;
		}
		reason = "'" + profileId + "' is switched off";
		return false;
	};
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntryWith(f, startsAt, true, {"p1", "p2", "p3"});

	f.clock = startsAt - History::kArmLeadMs;
	f.runner.Tick();

	// Three questions for three destinations, and exactly three: one sweep per tick
	// answers both the entry and every destination, so nothing is asked twice.
	assert_int_equal((int)asked.size(), 3);
	assert_string_equal(f.runner.DestinationBlockReason(id, "p1").c_str(), "");
	assert_string_equal(f.runner.DestinationBlockReason(id, "p2").c_str(), "'p2' is switched off");
	assert_string_equal(f.runner.DestinationBlockReason(id, "p3").c_str(), "'p3' is switched off");
}

// Cancelling clears the entry's own block reason; the per-destination half has to go
// with it. Nothing recomputes these once the row leaves `armed`, and a cancelled
// occurrence never re-arms, so one left behind sits on the chip for the whole
// retention window -- outliving the reconnect that made it untrue.
static void test_runner_cancel_clears_the_destination_refusals(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_cancel_refusals.db");
	f.runner.canArm = [](const std::string &profileId, std::string &reason) {
		if (profileId == "p1") {
			return true;
		}
		reason = "'" + profileId + "' is not connected";
		return false;
	};
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntryWith(f, startsAt, true, {"p1", "p2"});

	f.clock = startsAt - History::kArmLeadMs;
	f.runner.Tick();
	assert_string_equal(f.runner.DestinationBlockReason(id, "p2").c_str(), "'p2' is not connected");

	std::string error;
	assert_true(f.runner.CancelCountdown(id, error));
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kPlanned);
	assert_string_equal(f.runner.DestinationBlockReason(id, "p2").c_str(), "");

	// And no later tick brings it back, which is the whole reason the clear had to
	// happen here: a cancelled occurrence never re-arms, so nothing recomputes it.
	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_string_equal(f.runner.DestinationBlockReason(id, "p2").c_str(), "");
}

// An entry may name the same destination twice. The apply already dedupes by profile;
// the armability pass has to as well, or one disconnected account is asked about twice
// and says its refusal twice over in the entry's reason.
static void test_runner_asks_a_duplicated_destination_once(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_duplicate_dest.db");
	int asked = 0;
	f.runner.canArm = [&asked](const std::string &profileId, std::string &reason) {
		asked++;
		reason = "'" + profileId + "' is not connected";
		return false;
	};
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntryWith(f, startsAt, true, {"p1", "p1"});

	f.clock = startsAt - History::kArmLeadMs;
	f.runner.Tick();
	assert_int_equal(asked, 1);
	assert_string_equal(f.runner.BlockReason(id).c_str(), "'p1' is not connected");
	assert_string_equal(f.runner.DestinationBlockReason(id, "p1").c_str(), "'p1' is not connected");
}

// The default: the entry goes live with the destinations that can route. What it
// could not reach has to be recorded somewhere, or going live with one of three is
// exactly as silent as the bug this replaced.
static void test_runner_starts_a_partial_entry_when_destinations_are_not_all_required(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_partial_lenient.db");
	f.requireAllDestinations = false;
	f.runner.canArm = [](const std::string &profileId, std::string &reason) {
		if (profileId == "p1") {
			return true;
		}
		reason = "'" + profileId + "' is switched off";
		return false;
	};
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntryWith(f, startsAt, true, {"p1", "p2"});

	f.clock = startsAt - History::kArmLeadMs;
	f.runner.Tick();
	// The entry as a whole can still run, so its own reason stays empty; the
	// destination that cannot is named beside itself.
	assert_string_equal(f.runner.BlockReason(id).c_str(), "");
	assert_string_equal(f.runner.DestinationBlockReason(id, "p1").c_str(), "");
	assert_string_equal(f.runner.DestinationBlockReason(id, "p2").c_str(), "'p2' is switched off");

	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyAttempts, 1);
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal(f.goLiveCalls, 1);
	assert_true(LoggedLike(f, "is going live without: 'p2' is switched off"));
}

// The same entry under the strict setting. One destination it cannot reach refuses
// the whole start, and the runner must never even ask to load the routing -- an
// applyCalls of zero cannot tell that apart from an apply that was refused.
static void test_runner_refuses_a_partial_entry_when_every_destination_is_required(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_partial_strict.db");
	f.requireAllDestinations = true;
	f.runner.canArm = [](const std::string &profileId, std::string &reason) {
		if (profileId == "p1") {
			return true;
		}
		reason = "'" + profileId + "' is switched off";
		return false;
	};
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntryWith(f, startsAt, true, {"p1", "p2"});

	f.clock = startsAt - History::kArmLeadMs;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_string_equal(f.runner.BlockReason(id).c_str(), "'p2' is switched off");
	assert_string_equal(f.runner.DestinationBlockReason(id, "p1").c_str(), "");
	assert_string_equal(f.runner.DestinationBlockReason(id, "p2").c_str(), "'p2' is switched off");

	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyAttempts, 0);
	assert_int_equal(f.goLiveCalls, 0);

	// And by hand, which runs through the same armability gate. The refusals it works
	// out are the ones already on record, so this must NOT push a repaint for them.
	const int changedBefore = f.changedCalls;
	std::string error;
	assert_false(f.runner.StartNow(id, error));
	assert_string_equal(error.c_str(), "'p2' is switched off");
	assert_int_equal(f.applyAttempts, 0);
	assert_int_equal(f.changedCalls, changedBefore);
}

// The lower half of the same rule: without the strict setting the entry is blocked
// only when nothing it names can route, and the reason names every one of them.
static void test_runner_blocks_an_entry_when_no_destination_can_route(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_partial_none.db");
	f.requireAllDestinations = false;
	f.runner.canArm = [](const std::string &profileId, std::string &reason) {
		reason = "'" + profileId + "' is switched off";
		return false;
	};
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntryWith(f, startsAt, true, {"p1", "p2"});

	f.clock = startsAt - History::kArmLeadMs;
	f.runner.Tick();
	assert_string_equal(f.runner.BlockReason(id).c_str(), "'p1' is switched off; 'p2' is switched off");

	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyAttempts, 0);
	assert_int_equal(f.goLiveCalls, 0);
}

// The explicit "start this now" path, on a planned entry nowhere near its arm
// window. It has to arm the row itself -- RequestStart only loads routing, it never
// changes state -- or NoteWentLive would find the row still `planned` and drop the
// edge.
static void test_runner_start_now_arms_and_starts_a_planned_entry(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_start_now_planned.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt - 60 * 60 * 1000;
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kPlanned);

	std::string error;
	assert_true(f.runner.StartNow(id, error));
	assert_true(error.empty());
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal(f.goLiveCalls, 1);
	assert_int_equal(f.changedCalls, 1);
	assert_int_equal((int)f.appliedProfiles.size(), 1);
	assert_string_equal(f.appliedProfiles[0].c_str(), "p1");

	f.runner.NoteWentLive();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kLive);
}

// A missed entry is still "has not run yet" as far as StartNow is concerned -- the
// row settling on the missed grace only stops the calendar calling it upcoming.
static void test_runner_start_now_starts_a_missed_entry(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_start_now_missed.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt + History::kMissedGraceMs + 1000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kMissed);

	std::string error;
	assert_true(f.runner.StartNow(id, error));
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal(f.goLiveCalls, 1);
}

// A countdown cancelled, then overridden by "go live now" -- a new intent, so the
// old cancellation must not still block it, and StartNow's own flag reset is what
// clears it rather than IsStartInFlight, which never saw a request go out.
static void test_runner_start_now_after_cancel_countdown_succeeds(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_start_now_after_cancel.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);

	std::string error;
	assert_true(f.runner.CancelCountdown(id, error));
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kPlanned);
	assert_true(f.runner.IsCountdownCanceled(id));

	assert_true(f.runner.StartNow(id, error));
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal(f.goLiveCalls, 1);
	assert_false(f.runner.IsCountdownCanceled(id));
}

// Never take the routing away from a broadcast that is already running, the same
// refusal the clock itself is held to.
static void test_runner_start_now_refuses_while_streaming(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_start_now_streaming.db");
	f.streaming = true;
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt - 60 * 60 * 1000;
	std::string error;
	assert_false(f.runner.StartNow(id, error));
	assert_string_equal(error.c_str(), History::kAlreadyStreamingReason);
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kPlanned);
	assert_int_equal(f.applyAttempts, 0);
	assert_int_equal(f.goLiveCalls, 0);
}

// A start already on its way up is committed. StartNow must refuse it exactly as
// CancelCountdown and schedule.delete do, not ask a second time on top of it.
static void test_runner_start_now_refuses_when_already_in_flight(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_start_now_in_flight.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 1);
	assert_true(f.runner.IsStartInFlight(id));

	std::string error;
	assert_false(f.runner.StartNow(id, error));
	assert_string_equal(error.c_str(), "that entry is already going live");
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal(f.goLiveCalls, 1);
}

// An apply refusal is a refused start, not a start onto whatever was there --
// StartNow reports it and stops rather than calling goLive over unloaded routing.
// The row still arms: StartNow's own armability check already passed, and the
// refusal is the platform's, not the schedule's.
static void test_runner_start_now_propagates_an_apply_refusal(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_start_now_apply_refused.db");
	f.applyOk = false;
	f.applyReason = "the routing could not be loaded";
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt - 60 * 60 * 1000;
	std::string error;
	assert_false(f.runner.StartNow(id, error));
	assert_string_equal(error.c_str(), "the routing could not be loaded");
	// It got as far as asking: the refusal reported is the apply's own, not a gate
	// upstream of it that never reached the routing at all.
	assert_int_equal(f.applyAttempts, 1);
	assert_int_equal(f.applyCalls, 0);
	assert_int_equal(f.goLiveCalls, 0);
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
}

// A start that came back refused and was then started again by hand. The occurrence's
// failed flag has to go with the rest of what the clock decided: left set, the new
// request reads as not in flight, and everything the in-flight window holds off lets
// go at once -- cancelling stops refusing it, the missed sweep settles the row under
// it, and the settle puts the routing back while the prelude is still working.
static void test_runner_start_now_clears_a_failed_start(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_start_now_after_failure.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.goLiveCalls, 1);

	f.runner.NoteStartFailed();
	assert_false(f.runner.IsStartInFlight(id));
	assert_int_equal(f.revertCalls, 1);

	std::string error;
	assert_true(f.runner.StartNow(id, error));
	assert_int_equal(f.applyCalls, 2);
	assert_int_equal(f.goLiveCalls, 2);
	assert_true(f.runner.IsStartInFlight(id));

	// Committed again, so everything that keys off the in-flight window holds.
	assert_false(f.runner.CancelCountdown(id, error));
	f.clock = startsAt + History::kMissedGraceMs + 1000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_int_equal(f.revertCalls, 1);
}

// A settled row is not a start waiting to happen. Without the refusal StartNow would
// arm a finished entry and go live on it again, filing a second broadcast as the same
// occurrence -- and the in-flight refusal above it cannot stand in, since a broadcast
// that has already ended is not in flight.
static void test_runner_start_now_refuses_a_settled_entry(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_start_now_settled.db");
	const int64_t startsAt = 10'000'000;
	const std::string done = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	f.streaming = true;
	f.runner.NoteWentLive();
	f.streaming = false;
	f.runner.NoteStoppedStreaming();
	assert_string_equal(f.store.Get(done)->state.c_str(), History::ScheduleState::kDone);
	// The broadcast is over, so the in-flight refusal above this one has nothing to
	// say: what refuses is the settled row.
	assert_false(f.runner.IsStartInFlight(done));

	std::string error;
	assert_false(f.runner.StartNow(done, error));
	assert_string_equal(error.c_str(), "that entry is done and cannot be started");
	assert_string_equal(f.store.Get(done)->state.c_str(), History::ScheduleState::kDone);
	assert_int_equal(f.applyAttempts, 1);
	assert_int_equal(f.goLiveCalls, 1);

	// The same refusal for a row settled as cancelled rather than run.
	const std::string canceled = SeedEntry(f, startsAt + 60'000, false);
	assert_true(f.store.SetState(canceled, History::ScheduleState::kCanceled));
	assert_false(f.runner.StartNow(canceled, error));
	assert_string_equal(error.c_str(), "that entry is canceled and cannot be started");
	assert_string_equal(f.store.Get(canceled)->state.c_str(), History::ScheduleState::kCanceled);
	assert_int_equal(f.applyAttempts, 1);
	assert_int_equal(f.goLiveCalls, 1);
}

// A row reading `live` that this runner is not holding -- one left behind by a process
// that went away mid-broadcast and was read back from the database, or one from before
// an attach cleared what the runner knew. The in-flight refusal keys off the live id
// the runner holds, not off the row, so it has nothing to say about this one: without
// the state check StartNow would arm a row that already reads live and start a second
// broadcast against it.
static void test_runner_start_now_refuses_a_live_entry(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_start_now_live_row.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);
	assert_true(f.store.SetState(id, History::ScheduleState::kLive));

	f.clock = startsAt - 60 * 60 * 1000;
	assert_false(f.runner.IsStartInFlight(id));

	std::string error;
	assert_false(f.runner.StartNow(id, error));
	assert_string_equal(error.c_str(), "that entry is live and cannot be started");
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kLive);
	assert_int_equal(f.applyAttempts, 0);
	assert_int_equal(f.goLiveCalls, 0);
}

// Broadcasting to nowhere is worse than not broadcasting, on the manual path as much
// as on the clock's. Nothing else refuses this one: StartNow clears the occurrence's
// own block reason on the way through, so its armability check is all that stands
// between an entry with no destinations and a go-live.
static void test_runner_start_now_refuses_an_entry_with_no_destinations(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_start_now_no_dests.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false, false);

	f.clock = startsAt - 60 * 60 * 1000;
	std::string error;
	assert_false(f.runner.StartNow(id, error));
	assert_string_equal(error.c_str(), "this entry has no destinations");
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kPlanned);
	assert_int_equal(f.applyAttempts, 0);
	assert_int_equal(f.goLiveCalls, 0);
}

// The same refusal for destinations that exist but cannot carry an output right now,
// which is the reading that changes minute to minute -- an account that disconnected
// since the entry was planned.
static void test_runner_start_now_refuses_when_no_destination_can_go_live(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_start_now_unarmable.db");
	f.runner.canArm = [](const std::string &, std::string &reason) {
		reason = "'Main' is not connected";
		return false;
	};
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt - 60 * 60 * 1000;
	std::string error;
	assert_false(f.runner.StartNow(id, error));
	assert_string_equal(error.c_str(), "'Main' is not connected");
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kPlanned);
	assert_int_equal(f.applyAttempts, 0);
	assert_int_equal(f.goLiveCalls, 0);
	// The refusal this worked out is what the destination chips read, and it is
	// recorded for a `planned` row the clock never refreshes -- so the push has to
	// happen here or the reasons sit in memory until something unrelated repaints.
	assert_int_equal(f.changedCalls, 1);
	assert_string_equal(f.runner.DestinationBlockReason(id, "p1").c_str(), "'Main' is not connected");
}

// Guards the RequestStart extraction: a cancelled occurrence must still refuse the
// clock's own auto-start, exactly as it did before StartIfDue's body moved into the
// shared helper. The row is put back to `armed` by hand because cancelling leaves it
// `planned`, and StartIfDue's state gate would then refuse the start before the
// cancelled-occurrence check this test exists for is ever reached.
static void test_runner_auto_start_still_refuses_a_cancelled_occurrence(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_auto_still_refuses_cancel.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);

	std::string error;
	assert_true(f.runner.CancelCountdown(id, error));
	assert_true(f.store.SetState(id, History::ScheduleState::kArmed));

	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyAttempts, 0);
	assert_int_equal(f.goLiveCalls, 0);
	// Still armed and still cancelled: what refused the start was the occurrence,
	// not the row having been put back to planned.
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_true(f.runner.IsCountdownCanceled(id));
}

// AdoptImminentArmed is what a manual go-live calls on itself before reading output
// bindings, so an entry sitting in its arm window is loaded rather than left for a
// bare "looks imminent" guess to claim without ever having been applied. It uses
// PrepareStart, not RequestStart -- goLive is the caller's own next step, not
// something adoption invokes on itself.
static void test_runner_adopt_imminent_armed_loads_it_for_the_golive(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_adopt_loads.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);

	f.runner.AdoptImminentArmed();
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal((int)f.appliedProfiles.size(), 1);
	assert_string_equal(f.appliedProfiles[0].c_str(), "p1");
	// Adoption never calls goLive itself -- that is the caller's own next step, and
	// calling it here would re-enter the manual go-live that is asking.
	assert_int_equal(f.goLiveCalls, 0);

	f.runner.NoteWentLive();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kLive);
}

// A cancelled countdown must not be resurrected by an unrelated manual press.
// Cancelling puts the row back to `planned` and forgets the arm, so ImminentArmedId is
// what has nothing to hand back -- AdoptImminentArmed carries no cancelled-occurrence
// check of its own, and this is what pins the gate that does the refusing.
static void test_runner_adopt_skips_a_cancelled_occurrence(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_adopt_cancelled.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);

	std::string error;
	assert_true(f.runner.CancelCountdown(id, error));
	assert_true(f.runner.IsCountdownCanceled(id));

	f.runner.AdoptImminentArmed();
	assert_int_equal(f.applyAttempts, 0);

	f.runner.NoteWentLive();
	// Left alone entirely: still planned, not live, and still reads as cancelled.
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kPlanned);
	assert_true(f.runner.IsCountdownCanceled(id));
}

// An occurrence that already asked to start is not an unrelated manual press's to
// claim either, even once its request has been given up on: the routing that request
// loaded has already been put back, and adopting would load it again underneath a
// go-live that has nothing to do with this entry. A refused start is the state that
// reaches this -- it ends the request while leaving the row armed and the occurrence
// in the map, so nothing upstream of the check refuses first.
static void test_runner_adopt_skips_an_occurrence_that_already_asked_to_start(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_adopt_requested.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyAttempts, 1);
	assert_int_equal(f.goLiveCalls, 1);

	f.runner.NoteStartFailed();
	assert_false(f.runner.IsStartInFlight(id));
	assert_int_equal(f.revertCalls, 1);
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);

	f.runner.AdoptImminentArmed();
	assert_int_equal(f.applyAttempts, 1);
	assert_string_equal(f.runner.ActiveEntryId().c_str(), "");
	assert_false(f.runner.IsStartInFlight(id));
}

// The auto-start and StartNow both set startingId_ before calling goLive, so a
// go-live either of them triggered already belongs to an entry -- adoption must not
// ask a second time on top of it. The second entry is what leaves that claim as the
// only thing standing: it is armed, unblocked, unclaimed and starts sooner, so it is
// what adoption would take instead. The clock is moved past the in-flight allowance
// without a tick so the first entry's own hold on the routing is not what refuses.
static void test_runner_adopt_is_a_noop_once_something_already_claimed_the_start(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_adopt_noop.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);
	const std::string earlier = SeedEntry(f, startsAt - 30'000, false);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(earlier)->state.c_str(), History::ScheduleState::kArmed);

	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyCalls, 1);
	assert_int_equal(f.goLiveCalls, 1);
	assert_true(f.runner.IsStartInFlight(id));
	assert_true(f.runner.BlockReason(earlier).empty());

	f.clock = startsAt + History::kStartInFlightMs + 1000;
	assert_false(f.runner.IsStartInFlight(id));

	f.runner.AdoptImminentArmed();
	assert_int_equal(f.applyAttempts, 1);
	// The broadcast still belongs to the entry that asked for it.
	assert_string_equal(f.runner.ActiveEntryId().c_str(), id.c_str());
}

// A broadcast that is already live owns the routing, and an armed entry nothing asked
// to start is not what a second manual press should load underneath it. Refused on two
// independent grounds -- the claim a live entry already holds, and the routing being
// held elsewhere -- so this pins the outcome rather than either one of them.
static void test_runner_adopt_is_a_noop_while_a_broadcast_is_live(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_adopt_live.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, true);
	const std::string earlier = SeedEntry(f, startsAt - 30'000, false);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	f.clock = startsAt;
	f.runner.Tick();
	assert_int_equal(f.applyCalls, 1);
	f.streaming = true;
	f.runner.NoteWentLive();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kLive);
	assert_string_equal(f.store.Get(earlier)->state.c_str(), History::ScheduleState::kArmed);

	f.runner.AdoptImminentArmed();
	assert_int_equal(f.applyAttempts, 1);
	assert_int_equal(f.revertCalls, 0);
	assert_string_equal(f.runner.ActiveEntryId().c_str(), id.c_str());
}

// An apply refusal during adoption must not claim the entry -- the caller's go-live
// still runs (adoption swallows the refusal), but nothing here asked for this entry,
// so the following NoteWentLive has no id to give the edge to.
static void test_runner_adopt_apply_refusal_leaves_it_unclaimed(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_adopt_apply_refused.db");
	f.applyOk = false;
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);

	f.runner.AdoptImminentArmed();
	// It got as far as asking, and the entry is unclaimed because the apply refused
	// -- not because adoption stopped short of the routing.
	assert_int_equal(f.applyAttempts, 1);
	assert_int_equal(f.applyCalls, 0);
	assert_string_equal(f.runner.ActiveEntryId().c_str(), "");
	assert_false(f.runner.IsStartInFlight(id));
	assert_true(LoggedLike(f, "could not adopt"));

	f.runner.NoteWentLive();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
}

// Adoption loads an entry's routing, and loading it under a broadcast already running
// would redirect that broadcast -- while the apply that opens with a revert would drop
// the applied entry's bookkeeping on the way. The entry armed while nothing was
// streaming, so it carries no block reason: the routing check is what refuses, not a
// reason left on the occurrence by an earlier tick.
static void test_runner_adopt_skips_an_entry_while_the_routing_is_held_elsewhere(void **state)
{
	(void)state;
	RunnerFixture f;
	OpenRunner(f, "runner_adopt_streaming.db");
	const int64_t startsAt = 10'000'000;
	const std::string id = SeedEntry(f, startsAt, false);

	f.clock = startsAt - 60'000;
	f.runner.Tick();
	assert_string_equal(f.store.Get(id)->state.c_str(), History::ScheduleState::kArmed);
	assert_true(f.runner.BlockReason(id).empty());

	f.streaming = true;
	f.runner.AdoptImminentArmed();
	assert_int_equal(f.applyAttempts, 0);
	assert_string_equal(f.runner.ActiveEntryId().c_str(), "");
	assert_false(LoggedLike(f, "adopted"));
}

// A ScheduledSetup over in-memory routing and metadata. The runner's own tests
// count the injected calls, which proves the runner asked at the right moment and
// nothing about what came back -- and a restore that silently puts back the wrong
// thing leaves the user's destinations rewritten with nothing to show for it. So
// these assert the state itself.
struct SetupFixture {
	std::vector<History::RoutingBinding> bindings;
	std::map<std::string, nlohmann::json> overrides;
	bool streaming = false;
	// Refuse to enable this one binding, standing in for any reason the real setter
	// can say no beyond the single-live-stream rule the fake models on its own. Only
	// a restore switches a binding back on, so this is the restore's refusal.
	std::string refuseEnableOf;
	// The same for a disable, which is the only flip an apply makes.
	std::string refuseDisableOf;
	// Flip this one binding and then report failure anyway, as the real setter does
	// when the save fails after it has already assigned the flag, stopped the output
	// and reconciled.
	std::string persistFailOf;
	int refusals = 0;
	int saves = 0;
	History::ScheduledSetup setup;
};

static void OpenSetup(SetupFixture &f)
{
	f.setup.isStreaming = [&f] {
		return f.streaming;
	};
	f.setup.routing.read = [&f] {
		return f.bindings;
	};
	// Models the refusal the real setter makes at bridge.cpp: one stream profile is
	// one RTMP key, so only one binding carrying it may be enabled. A fake that
	// always says yes cannot exercise a restore that loses the routing it was
	// restoring, which is the whole hazard the ordering here exists to avoid.
	f.setup.routing.write = [&f](const std::string &uuid, bool enabled) {
		History::RoutingBinding *target = nullptr;
		for (History::RoutingBinding &b : f.bindings) {
			if (b.uuid == uuid) {
				target = &b;
			}
		}
		if (!target) {
			return false;
		}
		if (uuid == (enabled ? f.refuseEnableOf : f.refuseDisableOf)) {
			f.refusals++;
			return false;
		}
		if (enabled) {
			for (const History::RoutingBinding &b : f.bindings) {
				if (b.uuid != uuid && b.enabled && b.profileId == target->profileId) {
					f.refusals++;
					return false;
				}
			}
		}
		target->enabled = enabled;
		return uuid != f.persistFailOf;
	};
	f.setup.metadata.read = [&f](const std::string &profileId) {
		const auto it = f.overrides.find(profileId);
		return it == f.overrides.end() ? nlohmann::json::object() : it->second;
	};
	f.setup.metadata.write = [&f](const std::string &profileId, const nlohmann::json &fields) {
		f.overrides[profileId] = fields;
	};
	f.setup.metadata.clear = [&f](const std::string &profileId) {
		f.overrides.erase(profileId);
	};
	f.setup.metadata.save = [&f] {
		f.saves++;
	};
}

static void SeedBindings(SetupFixture &f)
{
	f.bindings.push_back({"b1", "p1", false});
	f.bindings.push_back({"b2", "p2", true});
	f.bindings.push_back({"b3", "p3", true});
}

static bool EnabledIs(const SetupFixture &f, const char *uuid, bool enabled)
{
	for (const History::RoutingBinding &b : f.bindings) {
		if (b.uuid == uuid) {
			return b.enabled == enabled;
		}
	}
	return false;
}

static History::ScheduleDestination Dest(const char *profileId, const char *title)
{
	History::ScheduleDestination d;
	d.profileId = profileId;
	d.title = title;
	return d;
}

// The enabled set narrows to what the entry names, and the set the user had comes
// back whole.
static void test_setup_narrows_the_enabled_set_and_restores_it(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);

	std::string reason;
	assert_true(f.setup.Apply({Dest("p2", "")}, reason));
	assert_true(f.setup.IsApplied());
	assert_true(EnabledIs(f, "b1", false));
	assert_true(EnabledIs(f, "b2", true));
	assert_true(EnabledIs(f, "b3", false));

	f.setup.Revert();
	assert_false(f.setup.IsApplied());
	assert_true(EnabledIs(f, "b1", false));
	assert_true(EnabledIs(f, "b2", true));
	assert_true(EnabledIs(f, "b3", true));

	// A second revert must not re-assert a stale snapshot over whatever is there now.
	f.setup.Revert();
	assert_true(EnabledIs(f, "b2", true));
}

// The rule the whole feature turns on: a scheduled entry never switches a destination
// on. Switching a binding on whose canvas has no other enabled binding wakes that
// canvas and starts an encode the user deliberately turned off -- so an entry naming a
// switched-off destination goes live without it rather than reaching over and enabling
// it.
//
// Both halves of that rule are load-bearing and both are exercised here, because
// either one alone leaves the other free to widen the enabled set: the entry must
// resolve a profile to the binding that is ON rather than to whichever comes first,
// AND it must never flip one from off to on.
static void test_setup_never_switches_a_destination_on(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f); // b1 (p1) is off; b2 (p2) and b3 (p3) are on
	// p4 is bound twice with the dormant one first, which is where a pick that ignored
	// `enabled` would land -- taking the profile off the canvas it is actually on and
	// putting it on one the user had switched off.
	f.bindings.push_back({"b4a", "p4", false});
	f.bindings.push_back({"b4b", "p4", true});

	std::string reason;
	assert_true(f.setup.Apply({Dest("p2", ""), Dest("p1", ""), Dest("p4", "")}, reason));
	assert_true(f.setup.IsApplied());
	assert_true(EnabledIs(f, "b1", false));  // named by the entry, and still off
	assert_true(EnabledIs(f, "b4a", false)); // named by the entry, and still off
	assert_true(EnabledIs(f, "b4b", true));  // the binding p4 is actually on, kept
	assert_true(EnabledIs(f, "b2", true));
	assert_true(EnabledIs(f, "b3", false));

	// Nothing was switched on, so the restore has only the one disable to undo.
	f.setup.Revert();
	assert_true(EnabledIs(f, "b1", false));
	assert_true(EnabledIs(f, "b4a", false));
	assert_true(EnabledIs(f, "b4b", true));
	assert_true(EnabledIs(f, "b2", true));
	assert_true(EnabledIs(f, "b3", true));
}

// An entry that resolves to no enabled binding would narrow the enabled set to
// nothing: everything off the air, an apply reporting success, and a broadcast going
// nowhere. The runner refuses such an entry before this is reached, but an outcome
// this bad must not rest on a caller remembering to check.
//
// Two ways to resolve to nothing, and the guard has to catch both: naming only
// destinations that are switched off, and naming none at all. The second reaches
// Apply through a narrow door -- an entry armed while it still had destinations, all
// of them removed before a manual go-live -- and the disable pass does not care which
// door it came through.
static void test_setup_refuses_an_entry_with_nothing_switched_on(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);

	std::string reason;
	assert_false(f.setup.Apply({}, reason));
	assert_false(reason.empty());
	assert_false(f.setup.IsApplied());
	assert_true(EnabledIs(f, "b2", true));
	assert_true(EnabledIs(f, "b3", true));

	reason.clear();
	assert_false(f.setup.Apply({Dest("p1", "Friday Night")}, reason));
	assert_false(reason.empty());
	assert_false(f.setup.IsApplied());
	// Nothing was taken off the air on the way to refusing.
	assert_true(EnabledIs(f, "b1", false));
	assert_true(EnabledIs(f, "b2", true));
	assert_true(EnabledIs(f, "b3", true));
	assert_int_equal((int)f.overrides.count("p1"), 0);
}

// One binding changed by hand since the apply is the newer intent and is left
// alone. It must not strand the rest of the restore, which is what comparing the
// enabled set as a whole -- or giving up on the first binding that moved -- would do.
//
// Two bindings have to come off for that to be provable, and the hand-changed one has
// to be the FIRST of them: records are held in the order Apply flipped them, so a
// restore that bailed on the changed-since binding would exit with the other already
// put back and nothing to show for the bug.
static void test_setup_leaves_a_binding_changed_since_alone(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);
	f.bindings.push_back({"b4", "p4", true});

	std::string reason;
	assert_true(f.setup.Apply({Dest("p2", "")}, reason));
	assert_true(EnabledIs(f, "b3", false)); // flipped first
	assert_true(EnabledIs(f, "b4", false)); // and this one second
	f.setup.routing.write("b3", true);      // the user turned it back on

	f.setup.Revert();
	assert_true(EnabledIs(f, "b1", false));
	assert_true(EnabledIs(f, "b2", true));
	assert_true(EnabledIs(f, "b3", true));
	// The one the restore was still owed, reached only by carrying on past the
	// binding it had to leave alone.
	assert_true(EnabledIs(f, "b4", true));
}

// A binding deleted while the entry held it. The restore has nothing to put it back
// onto, and must let go rather than keep owing a write for a uuid that will never
// answer again -- which would leave the setup reading applied for the rest of the
// session, retrying that write on every later revert.
//
// This is the one case the leave-alone guard decides on its own. For every other, an
// apply records only disables, so a binding "changed since" can only have been set
// back to what it was and re-asserting it would land on the same value.
static void test_setup_lets_go_of_a_binding_that_disappeared(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);

	std::string reason;
	assert_true(f.setup.Apply({Dest("p2", "")}, reason));
	assert_true(EnabledIs(f, "b3", false));

	for (auto it = f.bindings.begin(); it != f.bindings.end(); ++it) {
		if (it->uuid == "b3") {
			f.bindings.erase(it);
			break;
		}
	}

	f.setup.Revert();
	assert_false(f.setup.IsApplied());
	assert_true(EnabledIs(f, "b2", true));
}

// The entry states what it wants changed. A field it does not carry keeps whatever
// the user had remembered, or an entry with only a title erases their category and
// tags every time it runs.
static void test_setup_merges_metadata_rather_than_replacing(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);
	const nlohmann::json before = {{"title", "Old night"},
				       {"category", {{"id", "509658"}, {"name", "Just Chatting"}}},
				       {"tags", nlohmann::json::array({"english"})}};
	f.overrides["p2"] = before;

	std::string reason;
	assert_true(f.setup.Apply({Dest("p2", "Friday Night")}, reason));
	assert_string_equal(f.overrides["p2"]["title"].get<std::string>().c_str(), "Friday Night");
	assert_string_equal(f.overrides["p2"]["category"]["id"].get<std::string>().c_str(), "509658");
	assert_int_equal((int)f.overrides["p2"]["tags"].size(), 1);

	f.setup.Revert();
	assert_true(f.overrides["p2"] == before);
}

// One profile bound on two canvases, only one of which may be enabled. The entry
// routes through the enabled one whatever the order of the list -- taking the first
// match instead would move the profile onto a dormant canvas, waking it and starting
// an encode, which is the one thing an entry may not do.
static void test_setup_takes_the_enabled_binding_of_a_profile_bound_twice(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	// The dormant one listed first, which is where a first-match pick would land.
	f.bindings.push_back({"canvas2", "p1", false});
	f.bindings.push_back({"canvas1", "p1", true});

	std::string reason;
	assert_true(f.setup.Apply({Dest("p1", "")}, reason));
	assert_true(EnabledIs(f, "canvas1", true));
	assert_true(EnabledIs(f, "canvas2", false));
	assert_int_equal(f.refusals, 0);

	// The entry's own binding was already on and nothing else is bound here, so the
	// apply flipped nothing -- and the restore must leave alone what it never recorded
	// rather than re-asserting a snapshot of the whole set.
	f.setup.Revert();
	assert_true(EnabledIs(f, "canvas1", true));
	assert_true(EnabledIs(f, "canvas2", false));
	assert_int_equal(f.refusals, 0);
}

// A routing that will not take is a refused start, not a half-applied one: going
// live on part of an entry broadcasts to destinations nobody scheduled.
static void test_setup_abandons_an_apply_the_routing_refused(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);
	// Listed last, so the disable of b3 lands before this one refuses -- an abandon
	// that put nothing back would leave b3 off the air with nobody holding a record.
	f.bindings.push_back({"b4", "p4", true});
	f.refuseDisableOf = "b4";

	std::string reason;
	assert_false(f.setup.Apply({Dest("p2", "Friday Night")}, reason));
	assert_false(reason.empty());
	assert_false(f.setup.IsApplied());
	// Everything it managed to change on the way is back where it was.
	assert_true(EnabledIs(f, "b1", false));
	assert_true(EnabledIs(f, "b2", true));
	assert_true(EnabledIs(f, "b3", true));
	assert_true(EnabledIs(f, "b4", true));
	assert_int_equal((int)f.overrides.count("p2"), 0);
}

// A seam that changes the binding and only then reports failure -- the save going
// wrong after the flag, the output stop and the reconcile have all happened. The
// change is real, so it is recorded whatever the result said. Reading the result
// as "nothing changed" would leave the user's destination switched off with
// nothing anywhere that knows to switch it back on.
static void test_setup_records_a_flip_the_seam_would_not_confirm(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);
	f.persistFailOf = "b2"; // enabled, and not one the entry names

	std::string reason;
	assert_true(f.setup.Apply({Dest("p3", "")}, reason));
	assert_true(EnabledIs(f, "b2", false));
	assert_true(EnabledIs(f, "b3", true));

	f.setup.Revert();
	assert_false(f.setup.IsApplied());
	assert_true(EnabledIs(f, "b1", false));
	assert_true(EnabledIs(f, "b2", true));
	assert_true(EnabledIs(f, "b3", true));
}

// A restore the routing refuses keeps its snapshot for the next call. Dropping it
// would leave the user's destinations rewritten with nothing left to put them back.
static void test_setup_retries_a_restore_the_routing_refused(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);
	// A second binding for the apply to take off the air, so "everything it could put
	// back is back" is about more than the one that was refused. Listed after b2, which
	// is the one refused, so the restore has to carry on past that refusal to reach it.
	f.bindings.push_back({"b4", "p4", true});

	std::string reason;
	assert_true(f.setup.Apply({Dest("p3", "")}, reason));
	assert_true(EnabledIs(f, "b2", false));
	assert_true(EnabledIs(f, "b4", false));

	f.refuseEnableOf = "b2";
	f.setup.Revert();
	assert_true(f.setup.IsApplied());
	assert_true(f.refusals > 0);
	// Everything it could put back is back; the one it could not is still owed.
	assert_true(EnabledIs(f, "b1", false));
	assert_true(EnabledIs(f, "b2", false));
	assert_true(EnabledIs(f, "b3", true));
	assert_true(EnabledIs(f, "b4", true));

	f.refuseEnableOf.clear();
	f.setup.Revert();
	assert_false(f.setup.IsApplied());
	assert_true(EnabledIs(f, "b2", true));
	assert_true(EnabledIs(f, "b3", true));
	assert_true(EnabledIs(f, "b4", true));
}

// Entries written before the category id column carry a name and no id. No
// provider reads the name, so sending the pair would replace a remembered id with
// an empty one and the broadcast would go live under no category at all.
static void test_setup_keeps_a_remembered_category_id(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);
	const nlohmann::json before = {{"category", {{"id", "509658"}, {"name", "Just Chatting"}}}};
	f.overrides["p2"] = before;
	History::ScheduleDestination d = Dest("p2", "Friday Night");
	d.category = "Just Chatting";

	// The guard at the source, pinned on its own. The merge also skips empty strings,
	// so asserting only the outcome below leaves either of the two guards free to be
	// removed without a test noticing -- and the second removal would then surface far
	// from the change that caused it.
	assert_false(History::ScheduledSetup::MetadataFields(d).contains("category"));

	std::string reason;
	assert_true(f.setup.Apply({d}, reason));
	assert_string_equal(f.overrides["p2"]["category"]["id"].get<std::string>().c_str(), "509658");
	assert_string_equal(f.overrides["p2"]["title"].get<std::string>().c_str(), "Friday Night");

	f.setup.Revert();
	assert_true(f.overrides["p2"] == before);
}

// The same rule through the other half of the pair. An entry carrying a category id
// with no name -- a picker that reported only the id, or a name the provider never
// filled in -- must not blank the name the user had remembered. The remembered bag is
// deliberately older than what the entry carries, so an empty value in the entry is
// not an instruction to clear what is there.
static void test_setup_keeps_a_remembered_category_name(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);
	const nlohmann::json before = {{"category", {{"id", "509658"}, {"name", "Just Chatting"}}}};
	f.overrides["p2"] = before;
	History::ScheduleDestination d = Dest("p2", "Friday Night");
	d.categoryId = "743"; // the id the entry changes, carrying no name with it

	std::string reason;
	assert_true(f.setup.Apply({d}, reason));
	assert_string_equal(f.overrides["p2"]["category"]["id"].get<std::string>().c_str(), "743");
	assert_string_equal(f.overrides["p2"]["category"]["name"].get<std::string>().c_str(), "Just Chatting");

	f.setup.Revert();
	assert_true(f.overrides["p2"] == before);
}

// Providers key on the category id; the name is what a prefill shows. Both halves
// go through, neither standing in for the other.
static void test_setup_sends_the_category_id_and_name(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);
	History::ScheduleDestination d = Dest("p2", "");
	d.category = "Chess";
	d.categoryId = "743";
	d.tags = R"(["chess","live"])";

	std::string reason;
	assert_true(f.setup.Apply({d}, reason));
	assert_string_equal(f.overrides["p2"]["category"]["id"].get<std::string>().c_str(), "743");
	assert_string_equal(f.overrides["p2"]["category"]["name"].get<std::string>().c_str(), "Chess");
	assert_int_equal((int)f.overrides["p2"]["tags"].size(), 2);
	// No title on the destination, so none is written -- a blank must not blank one.
	assert_false(f.overrides["p2"].contains("title"));
}

// Two destinations naming the same profile. Captured once, or the second capture
// records what the first just wrote and the user's own value never comes back.
static void test_setup_captures_a_duplicate_profile_once(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);
	const nlohmann::json before = {{"title", "What the user typed"}};
	f.overrides["p2"] = before;

	std::string reason;
	assert_true(f.setup.Apply({Dest("p2", "First"), Dest("p2", "Second")}, reason));
	f.setup.Revert();
	assert_true(f.overrides["p2"] == before);
}

// An override the apply created, over a profile that had none, is removed rather
// than left behind as an empty-ish bag the go-live path would keep merging.
static void test_setup_removes_an_override_it_created(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);

	std::string reason;
	assert_true(f.setup.Apply({Dest("p2", "Friday Night")}, reason));
	assert_int_equal((int)f.overrides.count("p2"), 1);

	f.setup.Revert();
	assert_int_equal((int)f.overrides.count("p2"), 0);
}

// The worst outcome this feature could have is stopping a running broadcast.
// Flipping a binding off stops its output, so nothing here touches the routing
// while anything is live -- in either direction.
static void test_setup_never_touches_the_routing_while_live(void **state)
{
	(void)state;
	SetupFixture f;
	OpenSetup(f);
	SeedBindings(f);
	f.streaming = true;

	std::string reason;
	assert_false(f.setup.Apply({Dest("p2", "")}, reason));
	assert_string_equal(reason.c_str(), History::kAlreadyStreamingReason);
	assert_false(f.setup.IsApplied());
	assert_true(EnabledIs(f, "b3", true));

	// Applied while idle, then live: the restore waits for the stop edge and keeps
	// its snapshot rather than dropping it.
	f.streaming = false;
	assert_true(f.setup.Apply({Dest("p2", "")}, reason));
	f.streaming = true;
	f.setup.Revert();
	assert_true(f.setup.IsApplied());
	assert_true(EnabledIs(f, "b2", true));
	assert_true(EnabledIs(f, "b3", false));

	f.streaming = false;
	f.setup.Revert();
	assert_false(f.setup.IsApplied());
	assert_true(EnabledIs(f, "b2", true));
	assert_true(EnabledIs(f, "b3", true));
}

// A crash or a kill inside the armed window leaves rows describing what a process
// was doing. No process is doing it now: a row still `armed` would go live the
// moment the app opens, and one left `live` would read live forever, since only a
// broadcast's own stop edge settles it and that edge is gone.
static void test_schedule_recovers_interrupted_rows(void **state)
{
	(void)state;
	History::Db db;
	History::ScheduleStore store;
	const std::string path = TempDbPath("schedule_recover.db");
	assert_true(db.Open(path));
	assert_true(store.Attach(path));

	History::ScheduleEntry armed = MakeEntry(10'000'000, "armed one");
	assert_true(store.Create(armed, {}));
	assert_true(store.SetState(armed.id, History::ScheduleState::kArmed));
	History::ScheduleEntry live = MakeEntry(10'000'000, "live one");
	assert_true(store.Create(live, {}));
	assert_true(store.SetState(live.id, History::ScheduleState::kLive));
	History::ScheduleEntry planned = MakeEntry(20'000'000, "planned one");
	assert_true(store.Create(planned, {}));

	assert_int_equal(store.RecoverInterrupted(), 2);
	// Armed goes back to planned so the runner arms it again cleanly; live becomes
	// done, because it did happen -- the session row is where the crash is recorded.
	assert_string_equal(store.Get(armed.id)->state.c_str(), History::ScheduleState::kPlanned);
	assert_string_equal(store.Get(live.id)->state.c_str(), History::ScheduleState::kDone);
	assert_string_equal(store.Get(planned.id)->state.c_str(), History::ScheduleState::kPlanned);

	assert_int_equal(store.RecoverInterrupted(), 0);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_open_creates_database),
		cmocka_unit_test(test_open_sets_wal_and_version),
		cmocka_unit_test(test_migration_is_idempotent),
		cmocka_unit_test(test_v1_upgrades_to_current_preserving_rows),
		cmocka_unit_test(test_v2_upgrades_to_v3_preserving_rows),
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
		cmocka_unit_test(test_runner_applies_at_zero_not_at_arm),
		cmocka_unit_test(test_runner_holds_the_routing_until_a_start_is_given_up_on),
		cmocka_unit_test(test_runner_links_a_broadcast_that_came_up_after_the_grace),
		cmocka_unit_test(test_runner_puts_the_routing_back_when_the_go_live_is_refused),
		cmocka_unit_test(test_runner_guards_a_manually_started_entry),
		cmocka_unit_test(test_runner_refuses_a_start_while_another_is_on_its_way_up),
		cmocka_unit_test(test_runner_refuses_to_start_while_something_is_streaming),
		cmocka_unit_test(test_runner_refuses_a_start_when_the_routing_is_taken_mid_window),
		cmocka_unit_test(test_runner_refuses_a_start_the_apply_refused),
		cmocka_unit_test(test_runner_pushes_the_change_when_an_apply_refusal_blocks_a_start),
		cmocka_unit_test(test_runner_refuses_cancel_once_the_start_is_requested),
		cmocka_unit_test(test_runner_holds_the_routing_for_one_entry_at_a_time),
		cmocka_unit_test(test_runner_frees_a_given_up_start_for_deletion),
		cmocka_unit_test(test_runner_lets_go_of_a_deleted_entry_without_reverting),
		cmocka_unit_test(test_runner_lets_go_of_a_deleted_armed_entry),
		cmocka_unit_test(test_runner_disarms_an_entry_moved_out_of_its_window),
		cmocka_unit_test(test_runner_disarms_on_an_edit_without_waiting_for_a_tick),
		cmocka_unit_test(test_runner_tracks_armability_through_the_window),
		cmocka_unit_test(test_runner_asks_every_destination_not_just_the_first),
		cmocka_unit_test(test_runner_cancel_clears_the_destination_refusals),
		cmocka_unit_test(test_runner_asks_a_duplicated_destination_once),
		cmocka_unit_test(test_runner_starts_a_partial_entry_when_destinations_are_not_all_required),
		cmocka_unit_test(test_runner_refuses_a_partial_entry_when_every_destination_is_required),
		cmocka_unit_test(test_runner_blocks_an_entry_when_no_destination_can_route),
		cmocka_unit_test(test_runner_start_now_arms_and_starts_a_planned_entry),
		cmocka_unit_test(test_runner_start_now_starts_a_missed_entry),
		cmocka_unit_test(test_runner_start_now_after_cancel_countdown_succeeds),
		cmocka_unit_test(test_runner_start_now_refuses_while_streaming),
		cmocka_unit_test(test_runner_start_now_refuses_when_already_in_flight),
		cmocka_unit_test(test_runner_start_now_propagates_an_apply_refusal),
		cmocka_unit_test(test_runner_start_now_clears_a_failed_start),
		cmocka_unit_test(test_runner_start_now_refuses_a_settled_entry),
		cmocka_unit_test(test_runner_start_now_refuses_a_live_entry),
		cmocka_unit_test(test_runner_start_now_refuses_an_entry_with_no_destinations),
		cmocka_unit_test(test_runner_start_now_refuses_when_no_destination_can_go_live),
		cmocka_unit_test(test_runner_auto_start_still_refuses_a_cancelled_occurrence),
		cmocka_unit_test(test_runner_adopt_imminent_armed_loads_it_for_the_golive),
		cmocka_unit_test(test_runner_adopt_skips_a_cancelled_occurrence),
		cmocka_unit_test(test_runner_adopt_skips_an_occurrence_that_already_asked_to_start),
		cmocka_unit_test(test_runner_adopt_is_a_noop_once_something_already_claimed_the_start),
		cmocka_unit_test(test_runner_adopt_is_a_noop_while_a_broadcast_is_live),
		cmocka_unit_test(test_runner_adopt_apply_refusal_leaves_it_unclaimed),
		cmocka_unit_test(test_runner_adopt_skips_an_entry_while_the_routing_is_held_elsewhere),
		cmocka_unit_test(test_setup_narrows_the_enabled_set_and_restores_it),
		cmocka_unit_test(test_setup_never_switches_a_destination_on),
		cmocka_unit_test(test_setup_refuses_an_entry_with_nothing_switched_on),
		cmocka_unit_test(test_setup_leaves_a_binding_changed_since_alone),
		cmocka_unit_test(test_setup_takes_the_enabled_binding_of_a_profile_bound_twice),
		cmocka_unit_test(test_setup_abandons_an_apply_the_routing_refused),
		cmocka_unit_test(test_setup_records_a_flip_the_seam_would_not_confirm),
		cmocka_unit_test(test_setup_retries_a_restore_the_routing_refused),
		cmocka_unit_test(test_setup_lets_go_of_a_binding_that_disappeared),
		cmocka_unit_test(test_setup_keeps_a_remembered_category_id),
		cmocka_unit_test(test_setup_keeps_a_remembered_category_name),
		cmocka_unit_test(test_setup_merges_metadata_rather_than_replacing),
		cmocka_unit_test(test_setup_sends_the_category_id_and_name),
		cmocka_unit_test(test_setup_captures_a_duplicate_profile_once),
		cmocka_unit_test(test_setup_removes_an_override_it_created),
		cmocka_unit_test(test_setup_never_touches_the_routing_while_live),
		cmocka_unit_test(test_schedule_recovers_interrupted_rows),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
