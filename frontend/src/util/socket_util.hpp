#ifndef OBS_MULTISTREAM_FRONTEND_SOCKET_UTIL_HPP_
#define OBS_MULTISTREAM_FRONTEND_SOCKET_UTIL_HPP_

#include <cstddef>
#include <cstdint>

// Socket helpers shared by the app's loopback servers: the overlay server, the MCP server
// and the OAuth callback listener. Sockets are passed as uintptr_t (a SOCKET's width) so this
// header stays winsock-free, as overlay_server.hpp does.
namespace SocketUtil {

// Blocking write of an entire buffer; false on any send failure. Each send is bounded only by
// the caller's SO_SNDTIMEO, which the overlay server sets and the MCP server and OAuth listener
// do not.
bool SendAll(uintptr_t sock, const char *data, size_t len);

// SendAll for a response the caller closes next: it also waits, bounded, until the peer has
// acknowledged every byte, so none is still buffered when the socket closes.
bool SendResponse(uintptr_t sock, const char *data, size_t len);

} // namespace SocketUtil

#endif // OBS_MULTISTREAM_FRONTEND_SOCKET_UTIL_HPP_
