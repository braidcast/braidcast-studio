#include "account_store.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>

#include <util/platform.h>

#include "../log.hpp"

namespace OAuth {

namespace {

json AccountToJson(const OAuthAccount &a)
{
	return json{
		{"providerId", a.providerId}, {"access", a.access},
		{"refresh", a.refresh},       {"userId", a.userId},
		{"login", a.login},           {"displayName", a.displayName},
		{"expireTime", a.expireTime}, {"scopeVer", a.scopeVer},
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
	if (!CryptProtectData(&in, L"braidcast oauth", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
			      &blob)) {
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
	char buf[512];
	if (os_get_config_path(buf, sizeof(buf), "braidcast/oauth_tokens.json") <= 0) {
		return std::string();
	}
	return std::string(buf);
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

	std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
	if (!f) {
		return; // first run: no file yet
	}
	const std::vector<unsigned char> wrapped((std::istreambuf_iterator<char>(f)),
						 std::istreambuf_iterator<char>());
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
