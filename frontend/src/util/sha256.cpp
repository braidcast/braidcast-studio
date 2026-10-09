#include "util/sha256.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace Sha256 {

namespace {

constexpr size_t kFileChunk = 1 << 20;
constexpr ULONG kMaxUpdate = 0x7fffffff;

} // namespace

Hasher::Hasher()
{
	BCRYPT_ALG_HANDLE alg = nullptr;
	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
		return;
	}
	BCRYPT_HASH_HANDLE hash = nullptr;
	if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
		BCryptCloseAlgorithmProvider(alg, 0);
		return;
	}
	alg_ = alg;
	hash_ = hash;
	ok_ = true;
}

Hasher::~Hasher()
{
	if (hash_) {
		BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
	}
	if (alg_) {
		BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(alg_), 0);
	}
}

bool Hasher::Ok() const
{
	return ok_;
}

void Hasher::Update(const void *data, size_t n)
{
	auto *p = static_cast<PUCHAR>(const_cast<void *>(data));
	while (ok_ && n > 0) {
		const ULONG part = n > kMaxUpdate ? kMaxUpdate : static_cast<ULONG>(n);
		ok_ = BCRYPT_SUCCESS(BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_), p, part, 0));
		p += part;
		n -= part;
	}
}

bool Hasher::Final(unsigned char out[32])
{
	if (!ok_) {
		return false;
	}
	ok_ = false; // a CNG hash handle cannot be reused after finishing
	return BCRYPT_SUCCESS(BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), out, 32, 0));
}

bool Digest(const std::string &in, unsigned char out[32])
{
	Hasher h;
	h.Update(in.data(), in.size());
	return h.Final(out);
}

std::string ToHex(const unsigned char digest[32])
{
	static const char hex[] = "0123456789abcdef";
	std::string out;
	out.reserve(64);
	for (int i = 0; i < 32; ++i) {
		out.push_back(hex[digest[i] >> 4]);
		out.push_back(hex[digest[i] & 0x0f]);
	}
	return out;
}

bool FileHex(const std::string &utf8Path, std::string &hexOut, const std::atomic<bool> *cancel)
{
	std::ifstream in(std::filesystem::u8path(utf8Path), std::ios::binary);
	if (!in) {
		return false;
	}
	Hasher h;
	std::vector<char> buf(kFileChunk);
	while (in) {
		if (cancel && cancel->load(std::memory_order_acquire)) {
			return false;
		}
		in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
		const std::streamsize got = in.gcount();
		if (got > 0) {
			h.Update(buf.data(), static_cast<size_t>(got));
		}
	}
	if (!in.eof()) {
		return false;
	}
	unsigned char d[32];
	if (!h.Final(d)) {
		return false;
	}
	hexOut = ToHex(d);
	return true;
}

} // namespace Sha256
