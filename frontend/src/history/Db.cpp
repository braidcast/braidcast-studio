#include "Db.hpp"

#include <sqlite3.h>
#include <sqlite_orm/sqlite_orm.h>

namespace History {
namespace {

// A placeholder schema, minimal on purpose: it exists so the ORM is instantiated
// by a translation unit the build actually compiles, which is what keeps its
// compile requirements honest as the target's flags move.
struct SchemaMeta {
	int version = 0;
};

auto MakeStorage(const std::string &path)
{
	using namespace sqlite_orm;
	return make_storage(path, make_table("schema_meta", make_column("version", &SchemaMeta::version)));
}

} // namespace

Db::~Db()
{
	Close();
}

bool Db::Open(const std::string &path)
{
	Close();
	const int rc = sqlite3_open_v2(path.c_str(), &handle_,
				       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
	if (rc != SQLITE_OK) {
		lastError_ = handle_ ? sqlite3_errmsg(handle_) : "out of memory opening the database";
		Close();
		return false;
	}
	path_ = path;
	lastError_.clear();
	return true;
}

void Db::Close()
{
	if (handle_) {
		sqlite3_close_v2(handle_);
		handle_ = nullptr;
	}
	path_.clear();
}

int Db::Version() const
{
	if (!handle_) {
		return 0;
	}
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(handle_, "PRAGMA user_version", -1, &stmt, nullptr) != SQLITE_OK) {
		return 0;
	}
	const int version = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
	sqlite3_finalize(stmt);
	return version;
}

} // namespace History
