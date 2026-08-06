#ifndef OBS_MULTISTREAM_FRONTEND_RANDOM_UTIL_HPP_
#define OBS_MULTISTREAM_FRONTEND_RANDOM_UTIL_HPP_

#include <cstddef>
#include <string>

// The one cryptographic random source in the frontend. Every secret the app mints --
// the MCP bearer token, a per-overlay-widget token, the OAuth PKCE verifier + CSRF
// nonce -- draws from here, so none of them can end up on a weaker primitive or on a
// fallback that makes them guessable.
namespace RandomUtil {

// Fill `buf` with `len` cryptographically strong bytes. Returns false when the OS RNG
// refuses, and the buffer's contents are then undefined -- a caller must abandon the
// secret rather than use what it holds.
bool Bytes(unsigned char *buf, size_t len);

// `bytes` random bytes rendered as lowercase hex (2 * bytes characters). Empty on RNG
// failure: an unguessable token is the whole point, so there is no weaker second
// choice to fall back to, and callers must treat empty as "no token exists".
std::string HexToken(size_t bytes);

} // namespace RandomUtil

#endif // OBS_MULTISTREAM_FRONTEND_RANDOM_UTIL_HPP_
