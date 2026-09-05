// RunOverlaySelfTest lives in its own winsock-clean TU (not obs_bootstrap.cpp): the
// self-test needs a real loopback CLIENT socket, and including <winsock2.h> after the
// <windows.h> that obs.h pulls into obs_bootstrap.cpp would drag in the conflicting
// winsock v1 header. obs_bootstrap.hpp is obs-free, so defining the ObsBootstrap
// member here keeps winsock isolated exactly as overlay_server.cpp does.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../log.hpp"
#include "../obs_bootstrap.hpp"
#include "util/file_util.hpp"
#include "overlay_server.hpp"
#include "overlay_store.hpp"
#include "overlay_template.hpp"

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

// Does `acc` hold a complete SSE frame named `eventName` whose data line contains
// `marker`? Matching the pair rather than either half keeps one channel's body from
// crediting another channel's name -- which is exactly the drift this covers.
bool NamedFrameArrived(const std::string &acc, const std::string &eventName, const std::string &marker)
{
	const std::string head = "event: " + eventName + "\ndata: ";
	size_t pos = acc.find(head);
	while (pos != std::string::npos) {
		const size_t start = pos + head.size();
		const size_t end = acc.find('\n', start);
		if (end == std::string::npos) {
			return false; // the frame is still arriving
		}
		if (acc.substr(start, end - start).find(marker) != std::string::npos) {
			return true;
		}
		pos = acc.find(head, end);
	}
	return false;
}

// Read into `acc` -- re-running `push` each round when one is given -- until `arrived`
// accepts what has accumulated, or the attempt budget runs out. The re-push covers the
// registration race: RunSse adds a socket to the broadcast registry only once its
// handshake is on the wire, so a first push can land before this client is a target. The
// caller's short per-recv timeout is what makes a budget of attempts a bounded wait.
bool PumpUntil(SOCKET s, std::string &acc, const std::function<void()> &push,
	       const std::function<bool(const std::string &)> &arrived)
{
	char buf[4096];
	for (int attempt = 0; attempt < 40; ++attempt) {
		if (push) {
			push();
		}
		if (arrived(acc)) {
			return true;
		}
		const int n = recv(s, buf, sizeof(buf), 0);
		if (n > 0) {
			acc.append(buf, (size_t)n);
		}
	}
	return arrived(acc);
}

} // namespace

void ObsBootstrap::RunOverlaySelfTest()
{
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	// In-memory only: InjectForTest never persists, so overlays.json is untouched.
	//
	// Forked rather than stock, so the served document is decided entirely by this widget
	// and the assertions below do not depend on which templates the rundir happens to
	// hold. `accent` carries an override and `bg` does not, which is what makes the
	// document check below cover both halves of the merge.
	Overlay::Widget w;
	w.id = "selftest-widget";
	w.token = "selftesttoken";
	w.name = "selftest";
	w.type = "alertbox";
	Overlay::CustomCode code;
	code.html = "<div id=\"a\"></div>";
	code.css = "#a{color:#fff}";
	code.js = "OBSOverlay.onEvent(function(e){});";
	code.fields = Overlay::json::array({
		Overlay::json{{"key", "accent"}, {"type", "color"}, {"label", "Accent"}, {"default", "#9147ff"}},
		Overlay::json{{"key", "bg"}, {"type", "color"}, {"label", "Background"}, {"default", "#101014"}},
	});
	w.custom = code;
	w.settings = Overlay::json{{"accent", "#00ff00"}};
	Overlay::Store().InjectForTest(w);

	// A private server on an ephemeral port, NOT Overlay::Server(). That matters beyond
	// isolation: this runs after bootstrap has already started the real one, and the
	// teardown below calls Stop() -- pointed at the singleton it would take the live
	// overlay server down for the rest of the session.
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

	// 1) GET the assembled document. Three things are asserted, because they come from
	// three different resolutions and a regression in one does not disturb the others:
	// the fork's own markup is served rather than the type's shipped template; a key the
	// widget overrides arrives at the override; and a key it does not arrives at the
	// schema's default rather than missing.
	bool docOk = false;
	{
		SOCKET c = DialLoopback(port);
		if (c != INVALID_SOCKET) {
			WriteAll(c, "GET /w/selftest-widget?t=selftesttoken HTTP/1.1\r\nHost: x\r\n\r\n");
			const std::string resp = RecvUntilClose(c);
			docOk = StatusOf(resp) == 200 && resp.find("window.__OVERLAY__") != std::string::npos &&
				resp.find("src=\"/runtime.js") != std::string::npos &&
				resp.find("<div id=\"a\"></div>") != std::string::npos &&
				resp.find("#a{color:#fff}") != std::string::npos &&
				resp.find("OBSOverlay.onEvent(function(e){});") != std::string::npos &&
				resp.find("\"accent\":\"#00ff00\"") != std::string::npos &&
				resp.find("\"bg\":\"#101014\"") != std::string::npos;
			closesocket(c);
		}
	}
	HostLog(std::string("[selftest] overlay document -> ") + (docOk ? "OK" : "MISMATCH"));

	// 2) Open an SSE client, 3) broadcast a synthetic event, assert the data: frame, then
	// 4) push one frame through every named channel and assert each arrives under its own
	// event name.
	//
	// `liveSinceMs` is the start the stream frame below claims, which is also the window
	// the backfill replay in step 6 is bounded to. Taken as now, so the event store holds
	// nothing inside it and the replay stays independent of the user's real history.
	const int64_t liveSinceMs = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
								 std::chrono::system_clock::now().time_since_epoch())
								 .count());
	bool sseHeaderOk = false;
	bool deliveryOk = false;
	bool channelsOk = false;
	bool noWindowOk = false;
	SOCKET sse = DialLoopback(port);
	if (sse != INVALID_SOCKET) {
		WriteAll(sse, "GET /w/selftest-widget/events?t=selftesttoken HTTP/1.1\r\nHost: x\r\n\r\n");
		std::string acc = RecvHeaders(sse);
		sseHeaderOk = StatusOf(acc) == 200 && acc.find("text/event-stream") != std::string::npos;
		// Nothing has gone live on this private server yet, so there is no window to
		// replay events over and no backfill frame may be built.
		noWindowOk = acc.find("event: backfill") == std::string::npos;

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

		deliveryOk = PumpUntil(
			sse, acc, [&] { server.BroadcastTo("selftest-widget", ev); },
			[](const std::string &a) {
				return a.find("data:") != std::string::npos &&
				       a.find("selftest-ovl-1") != std::string::npos;
			});

		// One row per named channel, so adding a channel is a row here rather than
		// another copy of the pump above -- which is how the four drifted out of coverage
		// in the first place. `marker` is a value unique to that channel's body.
		struct Channel {
			const char *name;
			std::string marker;
			std::function<void()> push;
		};
		const std::vector<Channel> kChannels = {
			{"chat", "selftest-chat-1",
			 [&] {
				 Overlay::json msg = Overlay::json::object();
				 msg["id"] = "selftest-chat-1";
				 msg["platform"] = "twitch";
				 msg["author"] = "selftest-ovl";
				 msg["text"] = "hello";
				 server.BroadcastChat(msg);
			 }},
			{"viewers", "selftest:viewers",
			 [&] {
				 Overlay::json counts = Overlay::json::object();
				 counts["perAccount"] = Overlay::json::object();
				 counts["perAccount"]["selftest:viewers"] = 1;
				 server.BroadcastViewers(counts);
			 }},
			{"channels", "selftest:channels",
			 [&] {
				 Overlay::json entry = Overlay::json::object();
				 entry["audienceCount"] = 3;
				 entry["audienceKind"] = "followers";
				 Overlay::json stats = Overlay::json::object();
				 stats["perAccount"] = Overlay::json::object();
				 stats["perAccount"]["selftest:channels"] = entry;
				 server.BroadcastChannelStats(stats);
			 }},
			{"stream", std::to_string(liveSinceMs),
			 [&] {
				 Overlay::json state = Overlay::json::object();
				 state["active"] = true;
				 state["startedAt"] = liveSinceMs;
				 state["destinations"] = Overlay::json::array();
				 server.BroadcastStreamState(state);
			 }},
		};
		channelsOk = true;
		for (const Channel &c : kChannels) {
			const bool got = PumpUntil(sse, acc, c.push, [&](const std::string &a) {
				return NamedFrameArrived(a, c.name, c.marker);
			});
			HostLog(std::string("[selftest] overlay channel ") + c.name + " -> " +
				(got ? "OK" : "MISMATCH"));
			channelsOk = channelsOk && got;
		}
		closesocket(sse);
	}
	HostLog(std::string("[selftest] overlay SSE header -> ") + (sseHeaderOk ? "OK" : "MISMATCH"));
	HostLog(std::string("[selftest] overlay SSE delivery -> ") + (deliveryOk ? "OK" : "MISMATCH"));
	HostLog(std::string("[selftest] overlay named channels -> ") + (channelsOk ? "OK" : "MISMATCH"));

	// 5) Replay on connect: a stream opened AFTER the pushes above gets each state
	// channel's last frame plus the bounded event backfill, and nothing from the channels
	// that carry moments rather than state.
	bool replayOk = false;
	bool replayScopeOk = false;
	{
		SOCKET fresh = DialLoopback(port);
		if (fresh != INVALID_SOCKET) {
			WriteAll(fresh, "GET /w/selftest-widget/events?t=selftesttoken HTTP/1.1\r\nHost: x\r\n\r\n");
			std::string acc = RecvHeaders(fresh);
			const DWORD rtoMs = 100;
			setsockopt(fresh, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rtoMs, sizeof(rtoMs));
			const bool gotChannels = PumpUntil(fresh, acc, nullptr, [](const std::string &a) {
				return NamedFrameArrived(a, "channels", "selftest:channels");
			});
			const bool gotStream = PumpUntil(fresh, acc, nullptr, [&](const std::string &a) {
				return NamedFrameArrived(a, "stream", std::to_string(liveSinceMs));
			});
			const bool gotBackfill = PumpUntil(fresh, acc, nullptr, [](const std::string &a) {
				return NamedFrameArrived(a, "backfill", "\"events\":");
			});
			replayOk = gotChannels && gotStream && gotBackfill;
			// A replayed chat message would put a moment back on screen as if it had just
			// happened, and a replayed viewer count would assert an audience that may no
			// longer be watching.
			replayScopeOk = acc.find("event: chat") == std::string::npos &&
					acc.find("event: viewers") == std::string::npos;
			closesocket(fresh);
		}
	}
	HostLog(std::string("[selftest] overlay replay on connect -> ") + (replayOk ? "OK" : "MISMATCH"));
	HostLog(std::string("[selftest] overlay replay scope -> ") + (noWindowOk && replayScopeOk ? "OK" : "MISMATCH"));

	// 6) Wrong token -> 403.
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
	const std::optional<std::string> ovOrig = FileUtil::ReadUtf8File(ovPath);
	const std::optional<std::string> ovOrigBak = FileUtil::ReadUtf8File(ovBak);

	const Overlay::Widget created = Overlay::Store().Create("selftest-ovl", "alertbox").value_or(Overlay::Widget{});
	const std::string createdId = created.id;
	const std::string createdUrl = Overlay::WidgetUrl(created, Overlay::Store().Port());
	const bool createOk = !created.id.empty() && !created.token.empty() && !createdUrl.empty();
	// A new widget owns no code: it resolves both its schema and its markup through the
	// type's shipped template, which is what makes a later template fix reach it. Gated
	// rather than merely reported -- a rundir whose templates did not stage is a broken
	// build, and this is the only assertion that covers resolving through a type.
	const Overlay::ResolvedWidget createdView = Overlay::Resolve(created);
	const bool stockOk = !created.IsForked() && !createdView.schema.empty() && !createdView.html.empty();
	HostLog(std::string("[selftest] overlays create -> ") + (createOk && stockOk ? "OK" : "MISMATCH") +
		(stockOk ? " (stock, template resolves)" : " (its type's template did not resolve)"));

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

	// Against the staged rundir, so this reads the shipped set of template directories
	// rather than a list maintained beside them. A type with no natural size is created at
	// the canvas resolution and drawn at fifteen times its design scale; naming it here is
	// what keeps that from being found on stream instead.
	const std::vector<std::string> unsized = Overlay::TypesMissingNaturalSize();
	const bool sizesOk = unsized.empty();
	std::string unsizedList;
	for (const std::string &type : unsized) {
		unsizedList += unsizedList.empty() ? "" : ", ";
		unsizedList += type;
	}
	HostLog(std::string("[selftest] overlay natural sizes -> ") +
		(sizesOk ? "OK" : "MISSING (" + unsizedList + ")"));

	server.Stop();
	// Leave the shared singleton clean: the real boot Server() must not serve this
	// injected test widget after the smoke run.
	Overlay::Store().RemoveForTest("selftest-widget");
	HostLog("[selftest] overlay cleanup -> server stopped");

	if (docOk && sseHeaderOk && deliveryOk && channelsOk && replayOk && noWindowOk && replayScopeOk && authOk &&
	    stockOk && sizesOk) {
		HostLog("[selftest] overlay -> document/SSE/channels/replay/auth/stock/sizes OK");
	} else {
		HostLog("[selftest] overlay -> FAILED (see step lines above)");
	}

	WSACleanup();
}
