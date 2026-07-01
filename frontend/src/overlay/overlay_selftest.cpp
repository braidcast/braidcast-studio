// RunOverlaySelfTest lives in its own winsock-clean TU (not obs_bootstrap.cpp): the
// self-test needs a real loopback CLIENT socket, and including <winsock2.h> after the
// <windows.h> that obs.h pulls into obs_bootstrap.cpp would drag in the conflicting
// winsock v1 header. obs_bootstrap.hpp is obs-free, so defining the ObsBootstrap
// member here keeps winsock isolated exactly as overlay_server.cpp does.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include "../log.hpp"
#include "../obs_bootstrap.hpp"
#include "overlay_server.hpp"
#include "overlay_store.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace {

SOCKET DialLoopback(int port)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		return INVALID_SOCKET;
	}
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((unsigned short)port);
	if (connect(s, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	return s;
}

bool WriteAll(SOCKET s, const std::string &data)
{
	size_t sent = 0;
	while (sent < data.size()) {
		const int n = send(s, data.data() + sent, (int)(data.size() - sent), 0);
		if (n <= 0) {
			return false;
		}
		sent += (size_t)n;
	}
	return true;
}

// Read the whole response (server sends Content-Length + Connection: close).
std::string RecvUntilClose(SOCKET s)
{
	std::string out;
	char buf[2048];
	while (true) {
		const int n = recv(s, buf, sizeof(buf), 0);
		if (n <= 0) {
			break;
		}
		out.append(buf, (size_t)n);
	}
	return out;
}

// Read only up to (and including) the header terminator; leftover bytes returned via
// `out` so a following SSE frame recv can continue where this stopped.
std::string RecvHeaders(SOCKET s)
{
	std::string out;
	char buf[512];
	while (out.find("\r\n\r\n") == std::string::npos) {
		const int n = recv(s, buf, sizeof(buf), 0);
		if (n <= 0) {
			break;
		}
		out.append(buf, (size_t)n);
	}
	return out;
}

int StatusOf(const std::string &resp)
{
	// "HTTP/1.1 <code> ..."
	const size_t sp = resp.find(' ');
	if (sp == std::string::npos) {
		return 0;
	}
	try {
		return std::stoi(resp.substr(sp + 1, 3));
	} catch (...) {
		return 0;
	}
}

} // namespace

void ObsBootstrap::RunOverlaySelfTest()
{
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	// In-memory only: InjectForTest never persists, so overlays.json is untouched.
	Overlay::Widget w;
	w.id = "selftest-widget";
	w.token = "selftesttoken";
	w.name = "selftest";
	w.type = "alertbox";
	w.html = "<div id=\"a\"></div>";
	w.css = "#a{color:#fff}";
	w.js = "OBSOverlay.onEvent(function(e){});";
	w.fields = Overlay::json::array({Overlay::json{
		{"key", "accent"}, {"type", "color"}, {"label", "Accent"}, {"default", "#9147ff"}, {"value", "#9147ff"}}});
	Overlay::Store().InjectForTest(w);

	Overlay::OverlayServer server;
	int port = 0;
	const bool ok = server.StartForTest(0, &port);
	HostLog(std::string("[selftest] overlay StartForTest -> ") +
		(ok ? ("listening port=" + std::to_string(port)) : "FAILED"));
	if (!ok) {
		HostLog("[selftest] overlay -> FAILED (StartForTest did not bind)");
		WSACleanup();
		return;
	}

	// 1) GET the assembled document.
	bool docOk = false;
	{
		SOCKET c = DialLoopback(port);
		if (c != INVALID_SOCKET) {
			WriteAll(c, "GET /w/selftest-widget?t=selftesttoken HTTP/1.1\r\nHost: x\r\n\r\n");
			const std::string resp = RecvUntilClose(c);
			docOk = StatusOf(resp) == 200 && resp.find("window.__OVERLAY__") != std::string::npos &&
				resp.find("src=\"/runtime.js") != std::string::npos;
			closesocket(c);
		}
	}
	HostLog(std::string("[selftest] overlay document -> ") + (docOk ? "OK" : "MISMATCH"));

	// 2) Open an SSE client, 3) broadcast a synthetic event, assert the data: frame.
	bool sseHeaderOk = false;
	bool deliveryOk = false;
	SOCKET sse = DialLoopback(port);
	if (sse != INVALID_SOCKET) {
		WriteAll(sse, "GET /w/selftest-widget/events?t=selftesttoken HTTP/1.1\r\nHost: x\r\n\r\n");
		std::string acc = RecvHeaders(sse);
		sseHeaderOk = StatusOf(acc) == 200 && acc.find("text/event-stream") != std::string::npos;

		// Per-attempt short recv timeout so a not-yet-registered socket just retries
		// (RunSse registers the socket right after sending headers -- avoid that race).
		const DWORD rtoMs = 100;
		setsockopt(sse, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rtoMs, sizeof(rtoMs));

		Events::NormalizedEvent ev;
		ev.id = "selftest-ovl-1";
		ev.platform = "twitch";
		ev.type = "follow";
		ev.ts = 1000;
		ev.actorName = "selftest-ovl";

		char buf[1024];
		for (int attempt = 0; attempt < 40 && !deliveryOk; ++attempt) {
			server.BroadcastTo("selftest-widget", ev);
			const int n = recv(sse, buf, sizeof(buf), 0);
			if (n > 0) {
				acc.append(buf, (size_t)n);
			}
			if (acc.find("data:") != std::string::npos &&
			    acc.find("selftest-ovl-1") != std::string::npos) {
				deliveryOk = true;
			}
		}
		closesocket(sse);
	}
	HostLog(std::string("[selftest] overlay SSE header -> ") + (sseHeaderOk ? "OK" : "MISMATCH"));
	HostLog(std::string("[selftest] overlay SSE delivery -> ") + (deliveryOk ? "OK" : "MISMATCH"));

	// 4) Wrong token -> 403.
	bool authOk = false;
	{
		SOCKET c = DialLoopback(port);
		if (c != INVALID_SOCKET) {
			WriteAll(c, "GET /w/selftest-widget?t=wrong HTTP/1.1\r\nHost: x\r\n\r\n");
			const std::string resp = RecvUntilClose(c);
			authOk = StatusOf(resp) == 403;
			closesocket(c);
		}
	}
	HostLog(std::string("[selftest] overlay auth -> ") + (authOk ? "OK" : "MISMATCH"));

	// --- Real store round-trip (Group 2 persistence) ------------------------
	// Snapshot the user's real overlays.json (+ .bak) so the create/delete below
	// exercise the persist / reload path without clobbering real widgets; restored
	// byte-identical at the end (mirrors RunEventSelfTest's discipline).
	auto snapshot = [](const std::string &pth) -> std::optional<std::string> {
		std::ifstream in(std::filesystem::u8path(pth), std::ios::binary);
		if (!in) {
			return std::nullopt;
		}
		return std::optional<std::string>(std::in_place, std::istreambuf_iterator<char>(in),
						  std::istreambuf_iterator<char>());
	};
	auto restore = [](const std::string &pth, const std::optional<std::string> &data) {
		if (data) {
			std::ofstream out(std::filesystem::u8path(pth), std::ios::binary | std::ios::trunc);
			out.write(data->data(), static_cast<std::streamsize>(data->size()));
		} else {
			std::error_code ec;
			std::filesystem::remove(std::filesystem::u8path(pth), ec);
		}
	};

	const std::string ovPath = Overlay::OverlayStore::FilePath();
	const std::string ovBak = ovPath + ".bak";
	const std::optional<std::string> ovOrig = snapshot(ovPath);
	const std::optional<std::string> ovOrigBak = snapshot(ovBak);

	const Overlay::Widget created = Overlay::Store().Create("selftest-ovl", "alertbox");
	const std::string createdId = created.id;
	const std::string createdUrl = Overlay::WidgetUrl(created, Overlay::Store().Port());
	const bool createOk = !created.id.empty() && !created.token.empty() && !createdUrl.empty();
	const bool seededOk = !created.fields.empty() && !created.html.empty();
	HostLog(std::string("[selftest] overlays create -> ") + (createOk ? "OK" : "MISMATCH") +
		(seededOk ? " (template seeded)" : " (template missing -- empty seed)"));

	bool listOk = false;
	for (const Overlay::Widget &cand : Overlay::Store().List()) {
		if (cand.id == createdId) {
			listOk = true;
			break;
		}
	}
	HostLog(std::string("[selftest] overlays list -> ") + (listOk ? "OK" : "MISMATCH"));

	bool persistOk = false;
	{
		Overlay::OverlayStore reloaded;
		persistOk = reloaded.Get(createdId).has_value();
	}
	HostLog(std::string("[selftest] overlays persist -> ") + (persistOk ? "OK" : "MISMATCH"));

	Overlay::Store().Delete(createdId);
	bool deleteOk = false;
	{
		Overlay::OverlayStore reloaded;
		deleteOk = !reloaded.Get(createdId).has_value();
	}
	HostLog(std::string("[selftest] overlays delete -> ") + (deleteOk ? "OK" : "MISMATCH"));

	restore(ovPath, ovOrig);
	restore(ovBak, ovOrigBak);

	server.Stop();
	// Leave the shared singleton clean: the real boot Server() must not serve this
	// injected test widget after the smoke run.
	Overlay::Store().RemoveForTest("selftest-widget");
	HostLog("[selftest] overlay cleanup -> server stopped");

	if (docOk && sseHeaderOk && deliveryOk && authOk) {
		HostLog("[selftest] overlay -> document/SSE/auth OK");
	} else {
		HostLog("[selftest] overlay -> FAILED (see step lines above)");
	}

	WSACleanup();
}
