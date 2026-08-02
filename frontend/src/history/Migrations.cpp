#include "Db.hpp"

#include <cstdio>
#include <iterator>
#include <utility>

namespace History {

namespace {

// Schema v1.
//
// `sessions` and `session_destinations` are entities the UI names and revisits,
// so each carries a TEXT id alongside created_at/updated_at. `session_health` is
// an append-only sample stream instead: its `t` is already the row's timestamp,
// nothing addresses a sample by identity, and it is the one table that grows
// throughout a broadcast rather than at its edges -- so it takes a plain
// autoincrement rowid and no created_at/updated_at pair, which would cost the
// most storage in exactly the table that would never read them. That rowid is
// spelled NOT NULL, which the rowid-alias form does not imply on its own, so the
// column says what is already true of it and Schema.hpp can map it to a plain
// integer.
//
// `updated_at` is maintained by trigger rather than by the writing code so a
// write path that forgets a helper cannot bypass it. The triggers issue an
// UPDATE against the table they fire on; SQLite leaves recursive triggers off
// unless asked, so that write does not re-enter them.
//
// Scheduling (`schedule`, `schedule_destinations`) belongs to a later phase and
// is absent rather than stubbed out here.
constexpr const char *kMigration1 = R"SQL(
CREATE TABLE sessions (
    id            TEXT PRIMARY KEY NOT NULL,
    created_at    INTEGER NOT NULL,
    updated_at    INTEGER NOT NULL,
    started_at    INTEGER NOT NULL,
    ended_at      INTEGER,
    end_reason    TEXT,
    schedule_id   TEXT,
    title         TEXT NOT NULL DEFAULT '',
    canvas_uuids  TEXT NOT NULL DEFAULT '[]',
    thumb_path    TEXT
);

CREATE INDEX idx_sessions_started_at ON sessions (started_at DESC);

CREATE TABLE session_destinations (
    id            TEXT PRIMARY KEY NOT NULL,
    created_at    INTEGER NOT NULL,
    updated_at    INTEGER NOT NULL,
    session_id    TEXT NOT NULL REFERENCES sessions (id) ON DELETE CASCADE,
    profile_id    TEXT NOT NULL DEFAULT '',
    platform      TEXT NOT NULL DEFAULT '',
    account_label TEXT NOT NULL DEFAULT '',
    title         TEXT NOT NULL DEFAULT '',
    category      TEXT NOT NULL DEFAULT '',
    tags          TEXT NOT NULL DEFAULT '[]',
    final_state   TEXT NOT NULL DEFAULT '',
    error         TEXT NOT NULL DEFAULT ''
);

CREATE INDEX idx_session_destinations_session ON session_destinations (session_id);

CREATE TABLE session_health (
    id             INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
    session_id     TEXT NOT NULL REFERENCES sessions (id) ON DELETE CASCADE,
    t              INTEGER NOT NULL,
    bitrate_kbps   INTEGER NOT NULL DEFAULT 0,
    dropped_frames INTEGER NOT NULL DEFAULT 0,
    congestion_pct REAL    NOT NULL DEFAULT 0,
    encode_skipped INTEGER NOT NULL DEFAULT 0,
    cpu_pct        REAL    NOT NULL DEFAULT 0
);

CREATE INDEX idx_session_health_session_t ON session_health (session_id, t);

CREATE TRIGGER trg_sessions_updated_at AFTER UPDATE ON sessions
BEGIN
    UPDATE sessions SET updated_at = CAST(unixepoch('now','subsec') * 1000 AS INTEGER) WHERE id = NEW.id;
END;

CREATE TRIGGER trg_session_destinations_updated_at AFTER UPDATE ON session_destinations
BEGIN
    UPDATE session_destinations SET updated_at = CAST(unixepoch('now','subsec') * 1000 AS INTEGER) WHERE id = NEW.id;
END;
)SQL";

struct Migration {
	int version;
	const char *sql;
};

// Appending a migration is one entry here plus a bump of kCurrentSchemaVersion.
constexpr Migration kMigrations[] = {
	{1, kMigration1},
};

// Forgetting the bump is silent otherwise: existing installs skip the new
// migration, and fresh ones stamp a version their schema does not match.
static_assert(kMigrations[std::size(kMigrations) - 1].version == kCurrentSchemaVersion,
	      "bump kCurrentSchemaVersion when appending a migration");

} // namespace

bool Db::RollbackWith(std::string reason)
{
	Exec("ROLLBACK");
	lastError_ = std::move(reason);
	return false;
}

// One transaction spans every pending migration and the version stamp, so a
// failure part-way cannot leave the stamp standing over a half-built schema:
// SQLite rolls DDL and `user_version` back with everything else.
//
// The version is read inside that transaction, not before it. Two builds can
// share one history file -- the portable rundir's config directory is a junction
// to the installed build's -- and nothing else locks the file this early, so a
// version read outside the transaction lets both processes decide to migrate.
// BEGIN IMMEDIATE plus the connection's busy timeout serialises them, and the
// loser then sees the winner's version and commits an empty transaction.
bool Db::Migrate()
{
	if (!Exec("BEGIN IMMEDIATE")) {
		return false;
	}
	const int from = Version();
	if (from > kCurrentSchemaVersion) {
		return RollbackWith("history database is newer than this build");
	}
	if (from == kCurrentSchemaVersion) {
		return Exec("COMMIT");
	}
	for (const Migration &m : kMigrations) {
		if (m.version <= from) {
			continue;
		}
		if (!Exec(m.sql)) {
			return RollbackWith(lastError_);
		}
	}
	// `PRAGMA user_version` accepts no bound parameter, so the value is
	// formatted in. It is a compile-time constant, never input.
	char pragma[64];
	snprintf(pragma, sizeof pragma, "PRAGMA user_version = %d", kCurrentSchemaVersion);
	if (!Exec(pragma)) {
		return RollbackWith(lastError_);
	}
	return Exec("COMMIT");
}

} // namespace History
