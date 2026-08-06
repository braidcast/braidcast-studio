#include "account_store.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>

#include <util/platform.h>

#include "../log.hpp"
#include "../multistream/StorePaths.hpp"
#include "util/file_util.hpp"

namespace OAuth {

namespace {

json AccountToJson(const OAuthAccount &a)
{
	return json{
		{"providerId", a.providerId},
		{"access", a.access},
		{"refresh", a.refresh},
		{"userId", a.userId},
		{"login", a.login},
		{"displayName", a.displayName},
		{"expireTime", a.expireTime},
		{"scopeVer", a.scopeVer},
		{"refreshDead", a.refreshDead},
		{"avatarUrl", a.avatarUrl},
		{"audienceCount", a.audienceCount},
		{"audienceKind", AudienceKindName(a.audienceKind)},
		{"audienceHidden", a.audienceHidden},
		{"audienceUpdatedNs", a.audienceUpdatedNs},
		{"reusableStreamIds", a.reusableStreamIds},
		{"quotaResetEpoch", a.quotaResetEpoch},
	};
}

OAuthAccount AccountFromJson(const json &j)
{
	OAuthAccount a;
	if (!j.is_object()) {
		return a;
	}
	a.providerId = j.value("providerId", std::string());
	a.access = j.value("access", std::string());
	a.refresh = j.value("refresh", std::string());
	a.userId = j.value("userId", std::string());
	a.login = j.value("login", std::string());
	a.displayName = j.value("displayName", std::string());
	a.expireTime = j.value("expireTime", static_cast<int64_t>(0));
	a.scopeVer = j.value("scopeVer", 0);
	a.refreshDead = j.value("refreshDead", false);
	a.avatarUrl = j.value("avatarUrl", std::string());
	a.audienceCount = j.value("audienceCount", static_cast<int64_t>(-1));
	a.audienceKind = AudienceKindFromName(j.value("audienceKind", std::string()));
	a.audienceHidden = j.value("audienceHidden", false);
	a.audienceUpdatedNs = j.value("audienceUpdatedNs", static_cast<int64_t>(0));
	// Tolerant like every other field: a hand-edited or mis-typed value must degrade to "no
	// exhaustion recorded" rather than throw out of a load meant to degrade to empty. Failing
	// open here is the cheap direction -- a wrong "fine" costs one request, a wrong
	// "exhausted" would silence every YouTube feature for up to a day.
	if (const auto q = j.find("quotaResetEpoch"); q != j.end() && q->is_number_integer()) {
		a.quotaResetEpoch = q->get<int64_t>();
	}
	// A record written before ingest streams became per-destination carries a single bare
	// "reusableStreamId". It cannot be attributed to a stream profile, and guessing an owner
	// would hand one destination a stream another is already bound to, so it is DISCARDED:
	// the next go-live inserts a fresh stream per destination. Reading only the new key does
	// that -- an unknown key in the stored object is ignored, so the old file still loads.
	if (const auto ids = j.find("reusableStreamIds"); ids != j.end() && ids->is_object()) {
		// Per-entry type check rather than a whole-map get<>: one hand-edited non-string
		// value would otherwise throw out of a load that is meant to degrade to "empty",
		// never to fail.
		for (const auto &entry : ids->items()) {
			if (entry.value().is_string()) {
				a.reusableStreamIds[entry.key()] = entry.value().get<std::string>();
			}
		}
	}
	return a;
}

// DPAPI-wrap `plain` for the current user. Returns false (and leaves `out`
// untouched) on failure.
bool ProtectBytes(const std::string &plain, std::vector<unsigned char> &out)
{
	DATA_BLOB in;
	in.cbData = static_cast<DWORD>(plain.size());
	in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.data()));

	DATA_BLOB blob = {};
	// CRYPTPROTECT_UI_FORBIDDEN: a background-thread Put must never block on a UI prompt.
	if (!CryptProtectData(&in, L"braidcast oauth", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &blob)) {
		return false;
	}
	out.assign(blob.pbData, blob.pbData + blob.cbData);
	LocalFree(blob.pbData);
	return true;
}

// DPAPI-unwrap `wrapped`. Returns false on failure (corrupt blob / host change).
bool UnprotectBytes(const std::vector<unsigned char> &wrapped, std::string &plain)
{
	DATA_BLOB in;
	in.cbData = static_cast<DWORD>(wrapped.size());
	in.pbData = const_cast<BYTE *>(wrapped.data());

	DATA_BLOB blob = {};
	if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &blob)) {
		return false;
	}
	plain.assign(reinterpret_cast<const char *>(blob.pbData), blob.cbData);
	LocalFree(blob.pbData);
	return true;
}

} // namespace

std::string AccountStore::FilePath()
{
	return BraidcastConfigPath("oauth_tokens.json");
}

void AccountStore::EnsureLoadedLocked()
{
	if (loaded_) {
		return;
	}
	loaded_ = true; // mark loaded regardless: a failed/missing read = empty store

	const std::string path = FilePath();
	if (path.empty()) {
		return;
	}

	std::vector<unsigned char> wrapped;
	if (!FileUtil::ReadBinaryFile(std::filesystem::u8path(path), wrapped)) {
		return; // first run: no file yet
	}
	if (wrapped.empty()) {
		return;
	}

	std::string plain;
	if (!UnprotectBytes(wrapped, plain)) {
		HostLog("[oauth] token store unwrap failed (corrupt or host changed); starting empty");
		return;
	}

	const json root = json::parse(plain, nullptr, false);
	if (root.is_discarded() || !root.is_object()) {
		HostLog("[oauth] token store JSON unparseable; starting empty");
		return;
	}
	for (auto it = root.begin(); it != root.end(); ++it) {
		accounts_[it.key()] = AccountFromJson(it.value());
	}
}

void AccountStore::SaveLocked()
{
	const std::string path = FilePath();
	if (path.empty()) {
		HostLog("[oauth] token store path unresolved; not saving");
		return;
	}

	json root = json::object();
	for (const auto &entry : accounts_) {
		root[entry.first] = AccountToJson(entry.second);
	}
	const std::string plain = root.dump();

	std::vector<unsigned char> wrapped;
	if (!ProtectBytes(plain, wrapped)) {
		HostLog("[oauth] token store DPAPI protect failed; not saving");
		return;
	}

	const std::filesystem::path fsPath = std::filesystem::u8path(path);
	const std::filesystem::path tmpPath = std::filesystem::u8path(path + ".tmp");
	os_mkdirs(fsPath.parent_path().u8string().c_str());

	// Atomic write: a crash mid-write must never corrupt the live blob (an
	// undecryptable file would silently drop every linked account). Write the
	// full blob to a sibling temp file, then atomically replace the real file.
	{
		std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
		if (!f) {
			HostLog("[oauth] token store open-for-write failed");
			return;
		}
		f.write(reinterpret_cast<const char *>(wrapped.data()), static_cast<std::streamsize>(wrapped.size()));
		f.flush();
		if (!f) {
			HostLog("[oauth] token store temp write failed");
			f.close();
			std::error_code ec;
			std::filesystem::remove(tmpPath, ec);
			return;
		}
	}

	// MOVEFILE_REPLACE_EXISTING handles the first-write case too (dst absent ->
	// plain rename); MOVEFILE_WRITE_THROUGH flushes the metadata to disk.
	if (!MoveFileExW(tmpPath.c_str(), fsPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		HostLog("[oauth] token store atomic replace failed");
		std::error_code ec;
		std::filesystem::remove(tmpPath, ec);
		return;
	}
}

std::optional<OAuthAccount> AccountStore::Get(const std::string &accountId)
{
	const std::lock_guard<std::mutex> guard(mutex_);
	EnsureLoadedLocked();
	auto it = accounts_.find(accountId);
	if (it == accounts_.end()) {
		return std::nullopt;
	}
	return it->second;
}

void AccountStore::Put(const std::string &accountId, const OAuthAccount &account)
{
	const std::lock_guard<std::mutex> guard(mutex_);
	EnsureLoadedLocked();
	accounts_[accountId] = account;
	SaveLocked();
}

bool AccountStore::UpdateExisting(const std::string &accountId, const OAuthAccount &account)
{
	const std::lock_guard<std::mutex> guard(mutex_);
	EnsureLoadedLocked();
	auto it = accounts_.find(accountId);
	if (it == accounts_.end()) {
		return false;
	}
	it->second = account;
	SaveLocked();
	return true;
}

void AccountStore::UpdateAudience(const std::string &accountId, int64_t count, AudienceKind kind, bool hidden,
				  int64_t updatedNs)
{
	// Field-scoped write: a background poller must never round-trip a whole record it
	// read ~90s ago, or it would clobber a token that a reactive 401-refresh rotated in
	// the meantime. Touch only the audience fields on the CURRENT stored record, and
	// never re-insert an account removed mid-poll.
	const std::lock_guard<std::mutex> guard(mutex_);
	EnsureLoadedLocked();
	auto it = accounts_.find(accountId);
	if (it == accounts_.end()) {
		return;
	}
	it->second.audienceCount = count;
	it->second.audienceKind = kind;
	it->second.audienceHidden = hidden;
	it->second.audienceUpdatedNs = updatedNs;
	SaveLocked();
}

bool AccountStore::SetRefreshDead(const std::string &accountId, bool dead)
{
	// Field-scoped for the same reason as UpdateAudience: the refresh path reaches here
	// holding a record it read before a blocking HTTP call, so writing the whole thing
	// back could clobber a token a concurrent flight rotated in the meantime. Touch only
	// the verdict on the CURRENT stored record, and never re-insert a removed account.
	const std::lock_guard<std::mutex> guard(mutex_);
	EnsureLoadedLocked();
	auto it = accounts_.find(accountId);
	if (it == accounts_.end() || it->second.refreshDead == dead) {
		return false;
	}
	it->second.refreshDead = dead;
	SaveLocked();
	return true;
}

void AccountStore::UpdateReusableStreamId(const std::string &accountId, const std::string &profileUuid,
					  const std::string &streamId)
{
	// Field-scoped for the same reason as UpdateAudience: the go-live worker reaches
	// here holding an account copy read before several blocking HTTP calls, so writing
	// the whole record back could clobber a token a concurrent refresh rotated in the
	// meantime. Touch only THIS DESTINATION's entry on the CURRENT stored record -- two
	// profiles going live at once must not drop each other's stream -- and never
	// re-insert a removed account.
	const std::lock_guard<std::mutex> guard(mutex_);
	EnsureLoadedLocked();
	auto it = accounts_.find(accountId);
	if (it == accounts_.end()) {
		return;
	}
	std::map<std::string, std::string> &ids = it->second.reusableStreamIds;
	const auto existing = ids.find(profileUuid);
	if (existing != ids.end() && existing->second == streamId) {
		return;
	}
	ids[profileUuid] = streamId;
	SaveLocked();
}

void AccountStore::SetQuotaReset(const std::string &accountId, int64_t epochSeconds)
{
	// Field-scoped for the same reason as UpdateAudience: this is reached from whichever
	// worker thread happened to receive the quota-exhausted response, holding no record of
	// its own, and a whole-record write-back could clobber a token a concurrent refresh
	// rotated. Touch only the verdict on the CURRENT stored record, and never re-insert a
	// removed account.
	const std::lock_guard<std::mutex> guard(mutex_);
	EnsureLoadedLocked();
	auto it = accounts_.find(accountId);
	if (it == accounts_.end() || it->second.quotaResetEpoch == epochSeconds) {
		return;
	}
	it->second.quotaResetEpoch = epochSeconds;
	SaveLocked();
}

void AccountStore::Remove(const std::string &accountId)
{
	const std::lock_guard<std::mutex> guard(mutex_);
	EnsureLoadedLocked();
	if (accounts_.erase(accountId) > 0) {
		SaveLocked();
	}
}

std::map<std::string, OAuthAccount> AccountStore::All()
{
	const std::lock_guard<std::mutex> guard(mutex_);
	EnsureLoadedLocked();
	return accounts_;
}

AccountStore &Accounts()
{
	static AccountStore store;
	return store;
}

} // namespace OAuth
