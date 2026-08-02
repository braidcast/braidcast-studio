#include "harness.hpp"

#include "history/Db.hpp"
#include "history/Schema.hpp"

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
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
