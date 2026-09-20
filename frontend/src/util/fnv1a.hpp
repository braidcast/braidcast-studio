#ifndef OBS_MULTISTREAM_FRONTEND_UTIL_FNV1A_HPP_
#define OBS_MULTISTREAM_FRONTEND_UTIL_FNV1A_HPP_

#include <cstdint>
#include <string_view>

// FNV-1a, 64-bit. Not a cryptographic hash and not used as one: it backs a cache
// validator and a mutex-name token, where the requirement is that different bytes
// give different digests often enough, cheaply, with no dependency.
inline uint64_t Fnv1a64(std::string_view bytes)
{
	uint64_t h = 1469598103934665603ull; // offset basis
	for (const char c : bytes) {
		h ^= (uint64_t)(unsigned char)c;
		h *= 1099511628211ull; // prime
	}
	return h;
}

#endif // OBS_MULTISTREAM_FRONTEND_UTIL_FNV1A_HPP_
