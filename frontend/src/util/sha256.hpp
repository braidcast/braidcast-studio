#ifndef OBS_MULTISTREAM_FRONTEND_SHA256_HPP_
#define OBS_MULTISTREAM_FRONTEND_SHA256_HPP_

#include <atomic>
#include <cstddef>
#include <string>

// SHA-256 over Windows CNG (BCrypt). The one hashing seam for the frontend: PKCE
// challenges (oauth/broker_strategy) and model-download verification (voice).
namespace Sha256 {

// Incremental hasher. Ok() is false if CNG refused to create the hash, in which case
// Update is a no-op and Final returns false. Not copyable; one per stream.
class Hasher {
public:
	Hasher();
	~Hasher();
	Hasher(const Hasher &) = delete;
	Hasher &operator=(const Hasher &) = delete;

	bool Ok() const;
	void Update(const void *data, size_t n);
	bool Final(unsigned char out[32]);

private:
	void *alg_ = nullptr;  // BCRYPT_ALG_HANDLE
	void *hash_ = nullptr; // BCRYPT_HASH_HANDLE
	bool ok_ = false;
};

// One-shot digest of `in`.
bool Digest(const std::string &in, unsigned char out[32]);

// Lowercase hex of a 32-byte digest.
std::string ToHex(const unsigned char digest[32]);

// Hash a whole file in 1 MiB reads. Returns false on a read error, or when `cancel`
// (optional) becomes true between reads.
bool FileHex(const std::string &utf8Path, std::string &hexOut, const std::atomic<bool> *cancel);

} // namespace Sha256

#endif // OBS_MULTISTREAM_FRONTEND_SHA256_HPP_
