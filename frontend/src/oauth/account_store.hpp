#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_ACCOUNT_STORE_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_ACCOUNT_STORE_HPP_

#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "provider.hpp"

// The OAuth account store: OAuthAccount records keyed by AccountId (providerId:userId),
// persisted to <config>/braidcast/oauth_tokens.json, DPAPI-wrapped at rest
// (CryptProtectData) so the access/refresh tokens are not on disk in plaintext.
// Touched from both the CEF UI thread (bridge methods) and detached OAuth worker
// threads, so every accessor is mutex-guarded. A corrupt/undecryptable file (host
// changed, tampering) is logged and treated as empty -- never fatal. One account
// record here may be referenced by several stream profiles (connect-once reuse).
namespace OAuth {

class AccountStore {
public:
	std::optional<OAuthAccount> Get(const std::string &accountId);
	void Put(const std::string &accountId, const OAuthAccount &account);
	// Overwrite an EXISTING account's whole record under the lock -- used by the refresh
	// path to persist a freshly rotated token. Unlike Put, it never re-inserts an
	// account removed mid-refresh: a disconnect that lands while a refresh is in flight
	// must stay removed, not be resurrected by the refresh's write-back. Returns false
	// (nothing written) when the account is absent.
	bool UpdateExisting(const std::string &accountId, const OAuthAccount &account);
	// Update only the audience fields of an existing account (background-thread safe;
	// never touches tokens). No-op if the account was removed. Persists on write.
	void UpdateAudience(const std::string &accountId, int64_t count, AudienceKind kind, bool hidden,
			    int64_t updatedNs);
	// Update only the dead-refresh-token verdict of an existing account (worker-thread
	// safe; never touches tokens). No-op if the account was removed. Returns true when
	// the flag actually changed, so the caller can push oauth.status only on a real
	// transition. Persists on change.
	bool SetRefreshDead(const std::string &accountId, bool dead);
	void Remove(const std::string &accountId);
	std::map<std::string, OAuthAccount> All();

	// <config>/braidcast/oauth_tokens.json (the DPAPI blob, not JSON text).
	static std::string FilePath();

private:
	void EnsureLoadedLocked();
	void SaveLocked();

	std::mutex mutex_;
	bool loaded_ = false;
	std::map<std::string, OAuthAccount> accounts_;
};

// Process-wide account store accessor (Meyers singleton), mirroring the other
// frontend stores' free-accessor shape.
AccountStore &Accounts();

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_ACCOUNT_STORE_HPP_
