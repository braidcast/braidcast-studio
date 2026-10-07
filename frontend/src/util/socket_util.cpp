#include <winsock2.h>
#include <mstcpip.h>

#include "socket_util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>

#include "../log.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace SocketUtil {

namespace {

constexpr size_t kSendChunkBytes = 64 * 1024;
// Bounds the wait on a dead or stalled peer, and so how long Stop() can wait on a connection
// thread caught in it; shorter than the overlay's 3 s send timeout.
constexpr std::chrono::milliseconds kDeliveryWait{1000};
// Yield rather than sleep for the first stretch: Sleep(1) can round up to a whole timer tick
// (~15.6 ms), which every small response would otherwise pay.
constexpr std::chrono::milliseconds kYieldPhase{2};

bool ReadTcpInfo(SOCKET sock, TCP_INFO_v0 &info)
{
	DWORD version = 0;
	DWORD returned = 0;
	return WSAIoctl(sock, SIO_TCP_INFO, &version, sizeof(version), &info, sizeof(info), &returned, nullptr,
			nullptr) == 0;
}

void LogNoTcpInfoOnce(int error)
{
	static std::atomic<bool> logged{false};
	if (!logged.exchange(true)) {
		HostLog("[net] SIO_TCP_INFO failed (" + std::to_string(error) +
			"); a response on a socket where it fails closes without waiting for delivery and may "
			"arrive cut short (logged once)");
	}
}

} // namespace

bool SendAll(uintptr_t sock, const char *data, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		const int chunk = (int)std::min(len - sent, kSendChunkBytes);
		const int n = send((SOCKET)sock, data + sent, chunk, 0);
		if (n <= 0) {
			return false;
		}
		sent += (size_t)n;
	}
	return true;
}

// On some machines loopback loses data still buffered at close, so this waits for the acknowledgement.
// Chosen over SO_SNDBUF=0, which slowed an 8 MiB overlay asset here from 0.11 s to 0.7-4.4 s.
bool SendResponse(uintptr_t sock, const char *data, size_t len)
{
	const SOCKET s = (SOCKET)sock;
	TCP_INFO_v0 info{};
	if (!ReadTcpInfo(s, info)) {
		LogNoTcpInfoOnce(WSAGetLastError());
		return SendAll(sock, data, len);
	}
	const ULONG64 target = info.BytesOut + len;
	if (!SendAll(sock, data, len)) {
		return false;
	}
	const auto start = std::chrono::steady_clock::now();
	while (ReadTcpInfo(s, info) && (info.BytesOut < target || info.BytesInFlight != 0)) {
		const auto waited = std::chrono::steady_clock::now() - start;
		if (waited >= kDeliveryWait) {
			DBG(LogCat::Net, "response of %zu bytes not acknowledged within %lld ms; closing anyway", len,
			    (long long)kDeliveryWait.count());
			break;
		}
		if (waited < kYieldPhase) {
			SwitchToThread();
		} else {
			Sleep(1);
		}
	}
	return true;
}

} // namespace SocketUtil
