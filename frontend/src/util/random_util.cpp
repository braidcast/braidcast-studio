#include "random_util.hpp"

#include <vector>

#include <windows.h>

#include <bcrypt.h>

namespace RandomUtil {

bool Bytes(unsigned char *buf, size_t len)
{
	if (buf == nullptr || len == 0) {
		return false;
	}
	return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::string HexToken(size_t bytes)
{
	std::vector<unsigned char> raw(bytes);
	if (!Bytes(raw.data(), raw.size())) {
		return std::string();
	}
	static const char kHex[] = "0123456789abcdef";
	std::string out;
	out.reserve(bytes * 2);
	for (unsigned char c : raw) {
		out.push_back(kHex[c >> 4]);
		out.push_back(kHex[c & 0xF]);
	}
	return out;
}

} // namespace RandomUtil
