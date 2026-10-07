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
#include <thread>
#include <vector>

#include "../log.hpp"
#include "../obs_bootstrap.hpp"
#include "util/file_util.hpp"
#include "util/selftest_paths.hpp"
#include "../events/event_model.hpp"
#include "util/time_util.hpp"
#include "overlay_scopes.hpp"
#include "overlay_server.hpp"
#include "overlay_store.hpp"
#include "overlay_template.hpp"
#include "../multistream/StreamState.hpp"

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
std::string RecvUntilClose(SOCKET s, int *recvError = nullptr)
{
	std::string out;
	char buf[2048];
	while (true) {
		const int n = recv(s, buf, sizeof(buf), 0);
		if (n <= 0) {
			if (n == SOCKET_ERROR && recvError) {
				*recvError = WSAGetLastError();
			}
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

// Declared Content-Length of a response, if its header block names one.
std::optional<size_t> ContentLengthOf(const std::string &resp)
{
	const size_t head = resp.find("\r\n\r\n");
	const size_t at = resp.find("\r\nContent-Length: ");
	if (head == std::string::npos || at == std::string::npos || at > head) {
		return std::nullopt;
	}
	try {
		return (size_t)std::stoull(resp.substr(at + 18, head - at - 18));
	} catch (...) {
		return std::nullopt;
	}
}

// One request/response on a fresh connection. `complete` is false when the header never ended or the body's
// length differs from the declared Content-Length; a 304 counts as complete whatever it carries, since the
// caller's own check judges a 304 that has a body. `transportFailed` marks a dial or send failure, which is
// not a cut-short response and is never retried.
struct FetchResult {
	std::string resp;
	bool complete = false;
	bool transportFailed = false;
	std::optional<size_t> expected; // declared Content-Length
	size_t got = 0;                 // body bytes received
	int recvError = 0;
};

FetchResult FetchOnce(int port, const std::string &request)
{
	FetchResult r;
	SOCKET c = DialLoopback(port);
	if (c == INVALID_SOCKET) {
		r.transportFailed = true;
		return r;
	}
	const DWORD timeoutMs = 3000;
	setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeoutMs, sizeof(timeoutMs));
	if (WriteAll(c, request)) {
		r.resp = RecvUntilClose(c, &r.recvError);
	} else {
		r.transportFailed = true;
	}
	closesocket(c);
	const size_t head = r.resp.find("\r\n\r\n");
	if (head == std::string::npos) {
		return r;
	}
	r.expected = ContentLengthOf(r.resp);
	r.got = r.resp.size() - (head + 4);
	r.complete = !r.expected || StatusOf(r.resp) == 304 || r.got == *r.expected;
	return r;
}

// Loopback can drop a tail segment on some machines; a cut-short response gets one retry.
FetchResult FetchWhole(int port, const std::string &request, const char *what)
{
	FetchResult r = FetchOnce(port, request);
	if (r.complete || r.transportFailed) {
		return r;
	}
	HostLog(std::string("[selftest] overlay ") + what + ": loopback response cut short (" + std::to_string(r.got) +
		" of " + (r.expected ? std::to_string(*r.expected) : std::string("?")) + " body bytes, recv error " +
		std::to_string(r.recvError) + "); retrying once");
	return FetchOnce(port, request);
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

// Open `path` as an SSE client and read what the server sends on connect until `arrived`
// accepts it. Returns everything read, headers included; empty when the dial failed.
std::string ConnectSse(int port, const std::string &path, const std::function<bool(const std::string &)> &arrived)
{
	SOCKET s = DialLoopback(port);
	if (s == INVALID_SOCKET) {
		return std::string();
	}
	WriteAll(s, "GET " + path + " HTTP/1.1\r\nHost: x\r\n\r\n");
	std::string acc = RecvHeaders(s);
	const DWORD rtoMs = 100;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rtoMs, sizeof(rtoMs));
	PumpUntil(s, acc, nullptr, arrived);
	closesocket(s);
	return acc;
}

// The broadcast tally's rules, on tallies this test owns: the latch (a drifting start moves
// nothing, a null start opens nothing, an end closes the window for good), the refold of what
// was broadcast just before the start, the raw sums per (platform, type, kind) with a YouTube
// milestone kept apart from a new member, the recentIds bound, no names or messages in a
// snapshot, the record a restart reads -- an open window closed at the last save, no ids,
// and a platform's rows dropped past its storage limit -- and the save still owed after a
// failed write or a held-back event, written once the debounce has passed and not before.
bool BroadcastTallyRules()
{
	const auto event = [](const std::string &id, const char *platform, const char *type, int64_t ts) {
		Events::NormalizedEvent e;
		e.id = id;
		e.platform = platform;
		e.type = type;
		e.ts = ts;
		e.actorName = "selftest-tally-name";
		e.message = "selftest-tally-message";
		return e;
	};
	const auto row = [](const Overlay::json &snap, const char *platform, const char *type, const char *kind) {
		for (const Overlay::json &r : snap["totals"]) {
			if (r["platform"] == platform && r["type"] == type && r["kind"] == kind) {
				return r;
			}
		}
		return Overlay::json();
	};
	const auto sums = [&row](const Overlay::json &snap, const char *platform, const char *type, const char *kind,
				 int64_t events, int64_t units, int64_t amount) {
		const Overlay::json r = row(snap, platform, type, kind);
		return r.is_object() && r["events"] == events && r["units"] == units && r["amount"] == amount;
	};

	Overlay::BroadcastTally t;
	const Overlay::json empty = t.Snapshot();
	const bool emptyOk = empty["since"].is_null() && empty["until"].is_null() && empty["totals"].empty() &&
			     empty["recentIds"].empty() && t.OpenSince() == 0;

	// Broadcast before any window: remembered, not counted. Then the start, a moment earlier
	// than the frame that reports it: the event after it is folded in, the one before is not.
	Events::NormalizedEvent gift = event("tally-gift-early", "twitch", "subgift", 900);
	gift.count = 5;
	t.Add(gift);
	gift.id = "tally-gift-late";
	gift.ts = 1100;
	t.Add(gift);
	const std::optional<Overlay::json> opened = t.OnStreamState(true, 1000, 1200);
	const bool refoldOk = opened && (*opened)["since"] == 1000 && (*opened)["until"].is_null() &&
			      sums(*opened, "twitch", "subgift", "", 1, 5, 0) &&
			      (*opened)["recentIds"] == Overlay::json::array({"tally-gift-late"});

	// Latched: a drifting start and a live frame with no start move nothing.
	const bool latchOk = !t.OnStreamState(true, 1050, 1300) && !t.OnStreamState(true, 0, 1300) &&
			     t.OpenSince() == 1000;

	Events::NormalizedEvent member = event("tally-member-new", "youtube", "member", 1300);
	t.Add(member);
	member.id = "tally-member-milestone";
	member.months = 3;
	t.Add(member);
	Events::NormalizedEvent cheer = event("tally-cheer", "twitch", "cheer", 1400);
	cheer.amount = 250;
	t.Add(cheer);
	const Overlay::json live = t.Snapshot();
	const std::string liveDump = live.dump();
	const bool sumsOk = sums(live, "youtube", "member", "new", 1, 1, 0) &&
			    sums(live, "youtube", "member", "milestone", 1, 1, 0) &&
			    sums(live, "twitch", "cheer", "", 1, 1, 250) && live["recentIds"].size() == 4 &&
			    liveDump.find("selftest-tally-name") == std::string::npos &&
			    liveDump.find("selftest-tally-message") == std::string::npos;

	// The end closes the window at that moment, for good: a late event with an early time
	// is not counted, and a second end frame moves nothing.
	const std::optional<Overlay::json> closed = t.OnStreamState(false, 0, 2000);
	t.Add(event("tally-after-end", "twitch", "cheer", 1500));
	const bool endOk = closed && (*closed)["until"] == 2000 && !t.OnStreamState(false, 0, 2100) &&
			   t.OpenSince() == 0 && sums(t.Snapshot(), "twitch", "cheer", "", 1, 1, 250);

	// A busy broadcast names only its most recent counted ids.
	Overlay::BroadcastTally busy;
	busy.OnStreamState(true, 1000, 1000);
	const size_t kBusy = Overlay::BroadcastTally::kRecentIds + 6;
	for (size_t i = 0; i < kBusy; ++i) {
		busy.Add(event("tally-busy-" + std::to_string(i), "kick", "follow", 1000 + (int64_t)i));
	}
	const Overlay::json busySnap = busy.Snapshot();
	const bool boundOk = busySnap["recentIds"].size() == Overlay::BroadcastTally::kRecentIds &&
			     busySnap["recentIds"].back() == "tally-busy-" + std::to_string(kBusy - 1) &&
			     sums(busySnap, "kick", "follow", "", (int64_t)kBusy, (int64_t)kBusy, 0);

	// A busy start: far more events than a small memory holds arrive between an output starting
	// and the frame that reports its start, and the window counts every one of them.
	Overlay::BroadcastTally early;
	const size_t kEarly = 200;
	for (size_t i = 0; i < kEarly; ++i) {
		early.Add(event("tally-early-" + std::to_string(i), "twitch", "follow", 5000 + (int64_t)i));
	}
	early.OnStreamState(true, 5000, 6000);
	const bool earlyOk = sums(early.Snapshot(), "twitch", "follow", "", (int64_t)kEarly, (int64_t)kEarly, 0);

	// The record a restart reads. One broadcast left open (a crash, or quitting mid-stream),
	// closed on load at its last save; one ended long ago, whose YouTube rows are past that
	// platform's storage limit while its Twitch rows stand.
	bool reloadOk = false;
	bool agedOk = false;
	const std::string path = SelfTest::ConfigPath("overlay-tally-selftest.json");
	const auto clear = [&path] {
		std::error_code ec;
		for (const char *suffix : {"", ".bak", ".tmp"}) {
			std::filesystem::remove(std::filesystem::u8path(path + suffix), ec);
		}
	};
	if (!path.empty()) {
		const int64_t now = TimeUtil::NowMs();
		clear();
		{
			Overlay::BroadcastTally saved;
			saved.Open(path);
			saved.OnStreamState(true, now - 60000, now - 60000);
			saved.Add(event("tally-saved-1", "twitch", "follow", now - 1000));
			saved.Flush();
		}
		const std::string onDisk = FileUtil::ReadUtf8File(path).value_or(std::string());
		Overlay::BroadcastTally loaded;
		loaded.Open(path);
		const Overlay::json back = loaded.Snapshot();
		reloadOk = back["since"] == now - 60000 && back["until"].is_number_integer() &&
			   back["until"].get<int64_t>() >= now - 60000 && sums(back, "twitch", "follow", "", 1, 1, 0) &&
			   back["recentIds"].empty() && loaded.OpenSince() == 0 && !onDisk.empty() &&
			   onDisk.find("tally-saved-1") == std::string::npos;

		clear();
		{
			Overlay::BroadcastTally saved;
			saved.Open(path);
			saved.OnStreamState(true, 1000, 1000);
			saved.Add(event("tally-aged-yt", "youtube", "superchat", 1100));
			saved.Add(event("tally-aged-tw", "twitch", "raid", 1200));
			saved.OnStreamState(false, 0, 2000);
		}
		Overlay::BroadcastTally aged;
		aged.Open(path);
		const Overlay::json old = aged.Snapshot();
		agedOk = old["since"] == 1000 && old["until"] == 2000 && sums(old, "twitch", "raid", "", 1, 1, 0) &&
			 !row(old, "youtube", "superchat", "").is_object();
		clear();
	}

	// A save that fails stays owed. The opening save cannot be written (the file's folder is
	// a file), the event after it is held back by the debounce, and once the folder can be
	// made the tally is not written again before kSaveDebounceMs -- a failing disk is not
	// retried in a loop -- and then is, with the held-back event in it.
	bool retryOk = false;
	const std::string blocker = SelfTest::ConfigPath("overlay-tally-selftest-blocker");
	if (!blocker.empty()) {
		namespace fs = std::filesystem;
		std::error_code ec;
		const fs::path blockerPath = fs::u8path(blocker);
		const std::string owedPath = blocker + "/overlay_tally.json";
		fs::remove_all(blockerPath, ec);
		std::ofstream(blockerPath) << "blocks a folder of this name";
		const int64_t now = TimeUtil::NowMs();
		{
			Overlay::BroadcastTally owed;
			owed.Open(owedPath);
			owed.OnStreamState(true, now - 1000, now);
			owed.Add(event("tally-owed-1", "twitch", "follow", now));
			fs::remove(blockerPath, ec);
			owed.SaveIfDue(now + 1);
			const bool waited = !fs::exists(fs::u8path(owedPath), ec);
			owed.SaveIfDue(now + Overlay::BroadcastTally::kSaveDebounceMs);
			Overlay::BroadcastTally back;
			back.Open(owedPath);
			retryOk = waited && sums(back.Snapshot(), "twitch", "follow", "", 1, 1, 0);
		}
		fs::remove_all(blockerPath, ec);
	}

	const bool ok = emptyOk && refoldOk && latchOk && sumsOk && endOk && boundOk && earlyOk && reloadOk && agedOk &&
			retryOk;
	HostLog(std::string("[selftest] overlay broadcast tally -> ") + (ok ? "OK" : "MISMATCH") +
		" (empty=" + (emptyOk ? "ok" : "bad") + " refold=" + (refoldOk ? "ok" : "bad") +
		" latch=" + (latchOk ? "ok" : "bad") + " sums=" + (sumsOk ? "ok" : "bad") +
		" end=" + (endOk ? "ok" : "bad") + " bound=" + (boundOk ? "ok" : "bad") +
		" early=" + (earlyOk ? "ok" : "bad") + " reload=" + (reloadOk ? "ok" : "bad") +
		" aged=" + (agedOk ? "ok" : "bad") + " retry=" + (retryOk ? "ok" : "bad") + ")");
	return ok;
}

// The broadcast-state projection feeding the tally, over snapshots shaped like the engine's: an
// output that is still connecting carries no start and opens no window; one that has started but
// whose uptime still reads 0 (typical of the first snapshot after its start) carries that start
// and opens the window at it; and the broadcast's start is its first output's.
bool StreamStartOpensWindow()
{
	const int64_t now = 1'800'000'000'000;
	const auto output = [](const char *uuid, MultistreamEngine::State state, bool started, uint64_t uptimeMs) {
		MultistreamEngine::OutputStats s;
		s.bindingUuid = uuid;
		s.platformKey = "youtube";
		s.state = state;
		s.started = started;
		s.uptimeMs = uptimeMs;
		return s;
	};
	const auto startOf = [](const Overlay::json &state) {
		const auto it = state.find("startedAt");
		return it != state.end() && it->is_number_integer() ? it->get<int64_t>() : 0;
	};

	Overlay::BroadcastTally tally;
	const Overlay::json connecting =
		StreamStateJson({output("a", MultistreamEngine::State::Connecting, false, 0)}, true, now - 900);
	const bool connectingOk =
		connecting["startedAt"].is_null() && connecting["destinations"][0]["startedAt"].is_null() &&
		!tally.OnStreamState(connecting["active"].get<bool>(), startOf(connecting), now - 900);

	const Overlay::json wentLive =
		StreamStateJson({output("a", MultistreamEngine::State::Live, true, 0)}, true, now);
	const std::optional<Overlay::json> opened = tally.OnStreamState(true, startOf(wentLive), now);
	const bool liveOk = startOf(wentLive) == now && wentLive["destinations"][0]["startedAt"] == now && opened &&
			    (*opened)["since"] == now && (*opened)["until"].is_null();

	const Overlay::json both = StreamStateJson({output("a", MultistreamEngine::State::Live, true, 5000),
						    output("b", MultistreamEngine::State::Live, true, 0)},
						   true, now + 5000);
	const bool firstOk = startOf(both) == now && !tally.OnStreamState(true, startOf(both), now + 5000) &&
			     tally.OpenSince() == now;

	const bool ok = connectingOk && liveOk && firstOk;
	HostLog(std::string("[selftest] overlay stream start -> ") + (ok ? "OK" : "MISMATCH") +
		" (connecting=" + (connectingOk ? "ok" : "bad") + " live=" + (liveOk ? "ok" : "bad") +
		" first=" + (firstOk ? "ok" : "bad") + ")");
	return ok;
}

// FileUtil::WriteBinaryFileAtomic, the write behind AddAsset, against a target another
// handle holds open without FILE_SHARE_DELETE -- what an antivirus filter driver does to a
// just-saved file for a few milliseconds. A hold shorter than os_safe_replace's retry
// window must end in a saved file; a hold longer than it must fail with the previous bytes
// intact and no temp file left behind.
//
// The hold starts before the temp file is written and flushed, so the first replace lands
// some flush time F into it; the retries then land at F+5, F+20, F+70 and F+220 ms. The brief
// hold outlasts the first attempts even on a slow flush, so it passes only if the retries
// work; the long one outlasts the whole window with room for a flush under load.
bool AtomicWriteSurvivesHeldTarget()
{
	constexpr std::chrono::milliseconds kBriefHold{120};
	constexpr std::chrono::milliseconds kLongHold{1000};

	const std::string path = SelfTest::ConfigPath("atomic-write-probe.bin");
	if (path.empty()) {
		HostLog("[selftest] overlay atomic write -> FAILED (no scratch path)");
		return false;
	}
	const std::filesystem::path fsPath = std::filesystem::u8path(path);
	const auto write = [&](const std::string &bytes) {
		return FileUtil::WriteBinaryFileAtomic(path, bytes.data(), bytes.size());
	};
	const auto contents = [&] {
		return FileUtil::ReadUtf8File(path).value_or(std::string());
	};
	// Opens the target the way a scanner would and closes it after `hold` on another thread.
	// False when the open itself failed, which would make the step below prove nothing.
	const auto holdOpen = [&](std::chrono::milliseconds hold, std::thread &releaser) {
		const HANDLE h = CreateFileW(fsPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
					     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			return false;
		}
		releaser = std::thread([h, hold] {
			std::this_thread::sleep_for(hold);
			CloseHandle(h);
		});
		return true;
	};

	const bool seeded = write("first") == FileUtil::AtomicWriteResult::Ok && contents() == "first";

	std::thread briefRelease;
	const bool briefHeld = holdOpen(kBriefHold, briefRelease);
	const FileUtil::AtomicWriteResult brief = write("second");
	if (briefRelease.joinable()) {
		briefRelease.join();
	}
	const bool briefOk = briefHeld && brief == FileUtil::AtomicWriteResult::Ok && contents() == "second";

	std::thread longRelease;
	const bool longHeld = holdOpen(kLongHold, longRelease);
	const FileUtil::AtomicWriteResult held = write("third");
	if (longRelease.joinable()) {
		longRelease.join();
	}
	std::error_code ec;
	const bool longOk = longHeld && held == FileUtil::AtomicWriteResult::ReplaceFailed && contents() == "second" &&
			    !std::filesystem::exists(std::filesystem::u8path(FileUtil::AtomicWriteTempPath(path)), ec);

	std::filesystem::remove(fsPath, ec);
	const bool ok = seeded && briefOk && longOk;
	HostLog(std::string("[selftest] overlay atomic write -> ") + (ok ? "OK" : "FAILED") +
		" (seed=" + (seeded ? "ok" : "bad") + " " + std::to_string(kBriefHold.count()) +
		"ms-hold=" + (briefOk ? "saved" : "bad") + " " + std::to_string(kLongHold.count()) +
		"ms-hold=" + (longOk ? "refused, old bytes kept" : "bad") + ")");
	return ok;
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
	// document check below cover both halves of the merge. `note` holds what would end the
	// bootstrap's <script> early if it reached the page as typed.
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
		Overlay::json{{"key", "note"}, {"type", "text"}, {"label", "Note"}, {"default", ""}},
	});
	w.custom = code;
	w.settings = Overlay::json{{"accent", "#00ff00"}, {"note", "</script><b>x"}};
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
	std::string docResp;
	{
		SOCKET c = DialLoopback(port);
		if (c != INVALID_SOCKET) {
			WriteAll(c, "GET /w/selftest-widget?t=selftesttoken HTTP/1.1\r\nHost: x\r\n\r\n");
			docResp = RecvUntilClose(c);
			const std::string &resp = docResp;
			docOk = StatusOf(resp) == 200 && resp.find("window.__OVERLAY__") != std::string::npos &&
				resp.find("src=\"/runtime.js") != std::string::npos &&
				resp.find("<div id=\"a\"></div>") != std::string::npos &&
				resp.find("#a{color:#fff}") != std::string::npos &&
				resp.find("OBSOverlay.onEvent(function(e){});") != std::string::npos &&
				resp.find("\"accent\":\"#00ff00\"") != std::string::npos &&
				resp.find("\"bg\":\"#101014\"") != std::string::npos &&
				resp.find("\"note\":\"<\\/script><b>x\"") != std::string::npos &&
				resp.find("</script><b>x") == std::string::npos;
			closesocket(c);
		}
	}
	HostLog(std::string("[selftest] overlay document -> ") + (docOk ? "OK" : "MISMATCH"));

	// 1b) The runtime is the only cacheable route that needs nothing written into the config
	// tree to exercise, so it is where the cache headers get their automated check -- they had
	// none at all, which is how /runtime.js kept shipping with no validator at all. Asserted end
	// to end, because a validator that is never honoured is worth nothing: a 200 carries one, a
	// replay of it gets 304, and that 304 still names the length a 200 would have sent while
	// sending no body (RFC 9110 SS8.6) -- the part a naive 304 gets wrong.
	bool cacheOk = false;
	{
		std::string etag;
		size_t bodyLen = 0;
		const FetchResult first = FetchWhole(
			port, "GET /runtime.js?t=selftesttoken HTTP/1.1\r\nHost: x\r\n\r\n", "runtime caching");
		const std::string &resp = first.resp;
		const size_t tagAt = resp.find("ETag: ");
		const size_t tagEnd = tagAt == std::string::npos ? std::string::npos : resp.find("\r\n", tagAt);
		if (StatusOf(resp) == 200 && tagEnd != std::string::npos &&
		    resp.find("\r\n\r\n") != std::string::npos &&
		    resp.find("Cache-Control: private, max-age=") != std::string::npos) {
			etag = resp.substr(tagAt + 6, tagEnd - tagAt - 6);
			bodyLen = first.got;
		}
		if (!etag.empty() && bodyLen > 0) {
			const FetchResult again = FetchWhole(port,
							     "GET /runtime.js?t=selftesttoken HTTP/1.1\r\nHost: x\r\n"
							     "If-None-Match: " +
								     etag + "\r\n\r\n",
							     "runtime caching");
			cacheOk = StatusOf(again.resp) == 304 && again.resp.find("\r\n\r\n") != std::string::npos &&
				  again.got == 0 &&
				  again.resp.find("Content-Length: " + std::to_string(bodyLen) + "\r\n") !=
					  std::string::npos;
		}
	}
	HostLog(std::string("[selftest] overlay runtime caching -> ") + (cacheOk ? "OK" : "MISMATCH"));

	// 1c) The day-long max-age above is only safe because the document names the runtime by
	// its content: the `v` on the URL it serves must be the hash of the bytes /runtime.js
	// sends, which is the ETag without its quotes. Otherwise a rebuild pairs a new template
	// with a cached old runtime.
	bool versionOk = false;
	{
		const FetchResult r = FetchWhole(port, "GET /runtime.js?t=selftesttoken HTTP/1.1\r\nHost: x\r\n\r\n",
						 "runtime version");
		const std::string &resp = r.resp;
		const size_t tagAt = resp.find("ETag: \"");
		const size_t tagEnd = tagAt == std::string::npos ? std::string::npos : resp.find("\"\r\n", tagAt + 7);
		if (StatusOf(resp) == 200 && tagEnd != std::string::npos && r.complete && r.expected) {
			const std::string hash = resp.substr(tagAt + 7, tagEnd - tagAt - 7);
			versionOk = !hash.empty() && docResp.find("src=\"/runtime.js?t=selftesttoken&v=" + hash +
								  "\"") != std::string::npos;
		}
	}
	HostLog(std::string("[selftest] overlay runtime version -> ") + (versionOk ? "OK" : "MISMATCH"));

	// 1d) The bundled sound library: served with no token and a public cache policy, and
	// nothing outside it however the path is spelled. A build without the pack (it is generated
	// by scripts/build-sound-library.py and committed on its own) checks only the guard, which
	// needs no file, and says the rest was skipped.
	bool libraryOk = false;
	std::error_code packEc;
	const bool packBuilt =
		std::filesystem::exists(std::filesystem::u8path(Overlay::LibraryRoot() + "/sounds.json"), packEc);
	{
		const auto get = [&](const std::string &target, const std::string &extraHeaders = std::string()) {
			SOCKET c = DialLoopback(port);
			if (c == INVALID_SOCKET) {
				return std::string();
			}
			WriteAll(c, "GET " + target + " HTTP/1.1\r\nHost: x\r\n" + extraHeaders + "\r\n");
			const std::string resp = RecvUntilClose(c);
			closesocket(c);
			return resp;
		};
		const std::string manifest = get("/lib/sounds.json");
		const bool manifestOk = StatusOf(manifest) == 200 &&
					manifest.find("Content-Type: application/json") != std::string::npos &&
					manifest.find("Cache-Control: public, max-age=") != std::string::npos &&
					manifest.find("\"chime-01\"") != std::string::npos;
		const std::string sound = get("/lib/sounds/chime-01.ogg");
		const bool soundOk = StatusOf(sound) == 200 &&
				     sound.find("Content-Type: audio/ogg") != std::string::npos;
		bool guardOk = true;
		for (const char *escape : {"/lib/../runtime.js", "/lib/sounds/../../runtime.js",
					   "/lib/%2e%2e/runtime.js", "/lib/sounds/..\\..\\runtime.js",
					   "/lib/C:/Windows/win.ini", "/lib/.hidden", "/lib/", "/lib/a/b/c.ogg"}) {
			guardOk = guardOk && StatusOf(get(escape)) == 404;
		}
		// Byte ranges, which a <video> needs to seek and loop: a span, a suffix, one past the
		// end, and a range asked against bytes that have since changed.
		const auto bodyOf = [](const std::string &resp) {
			const size_t head = resp.find("\r\n\r\n");
			return head == std::string::npos ? std::string() : resp.substr(head + 4);
		};
		const std::string whole = bodyOf(sound);
		const std::string size = std::to_string(whole.size());
		const std::string span = get("/lib/sounds/chime-01.ogg", "Range: bytes=0-9\r\n");
		const std::string suffix = get("/lib/sounds/chime-01.ogg", "Range: bytes=-5\r\n");
		const std::string past = get("/lib/sounds/chime-01.ogg", "Range: bytes=" + size + "-\r\n");
		const std::string stale =
			get("/lib/sounds/chime-01.ogg", "Range: bytes=0-9\r\nIf-Range: \"not-these-bytes\"\r\n");
		const bool rangeOk = whole.size() > 10 && sound.find("Accept-Ranges: bytes") != std::string::npos &&
				     StatusOf(span) == 206 && bodyOf(span) == whole.substr(0, 10) &&
				     span.find("Content-Range: bytes 0-9/" + size + "\r\n") != std::string::npos &&
				     StatusOf(suffix) == 206 && bodyOf(suffix) == whole.substr(whole.size() - 5) &&
				     StatusOf(past) == 416 &&
				     past.find("Content-Range: bytes */" + size + "\r\n") != std::string::npos &&
				     StatusOf(stale) == 200 && bodyOf(stale) == whole;
		if (packBuilt) {
			libraryOk = manifestOk && soundOk && guardOk && rangeOk;
			HostLog(std::string("[selftest] overlay sound library -> ") + (libraryOk ? "OK" : "MISMATCH") +
				" (manifest=" + (manifestOk ? "ok" : "bad") + " sound=" + (soundOk ? "ok" : "bad") +
				" guard=" + (guardOk ? "ok" : "bad") + " range=" + (rangeOk ? "ok" : "bad") + ")");
		} else {
			// The guard still means something: each escape names a file that exists outside
			// the library (runtime.js). And a missing manifest must be a plain 404.
			libraryOk = guardOk && StatusOf(manifest) == 404;
			HostLog(std::string("[selftest] overlay sound library -> ") +
				(libraryOk ? "SKIPPED (no sound pack; guard=ok)" : "MISMATCH (no sound pack)"));
		}
	}

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

		// A test is marked as one, so a page can keep it out of anything it keeps.
		deliveryOk = PumpUntil(
			sse, acc, [&] { server.BroadcastTo("selftest-widget", ev); },
			[](const std::string &a) {
				return a.find("data:") != std::string::npos &&
				       a.find("selftest-ovl-1") != std::string::npos &&
				       a.find("\"test\":true") != std::string::npos;
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
			{"moderation", "selftest-mod-1",
			 [&] {
				 Overlay::json op = Overlay::json::object();
				 op["platform"] = "twitch";
				 op["action"] = "message";
				 op["msgId"] = "selftest-mod-1";
				 op["before"] = 1;
				 server.BroadcastChatModeration(op);
			 }},
			{"eventredaction", "selftest-redacted-1",
			 [&] {
				 Overlay::json ids = Overlay::json::object();
				 ids["ids"] = Overlay::json::array({"selftest-redacted-1"});
				 server.BroadcastEventRedaction(ids);
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
				 // The go-live frame as the engine's first snapshot of a started output
				 // projects it: its uptime still 0.
				 MultistreamEngine::OutputStats out;
				 out.bindingUuid = "selftest-binding";
				 out.platformKey = "twitch";
				 out.state = MultistreamEngine::State::Live;
				 out.started = true;
				 server.BroadcastStreamState(StreamStateJson({out}, true, liveSinceMs));
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
			// happened, a replayed moderation op or event redaction names what a fresh page
			// never drew, and a replayed viewer count would assert an audience that may no
			// longer be watching.
			// The tally is a counting type's alone: every other page would only discard it.
			replayScopeOk = acc.find("event: chat") == std::string::npos &&
					acc.find("event: moderation") == std::string::npos &&
					acc.find("event: eventredaction") == std::string::npos &&
					acc.find("event: viewers") == std::string::npos &&
					acc.find("event: tally") == std::string::npos;
			closesocket(fresh);
		}
	}
	HostLog(std::string("[selftest] overlay replay on connect -> ") + (replayOk ? "OK" : "MISMATCH"));
	HostLog(std::string("[selftest] overlay replay scope -> ") + (noWindowOk && replayScopeOk ? "OK" : "MISMATCH"));

	// 5b) The counter's tally, over the SAME token gate as every /w/ route. On connect a
	// counting widget gets the open window's totals, which a live event counts and a test
	// frame never does. A connect racing a broadcast gets every event exactly where the
	// registration-before-read order puts it: in the tally, or live after it, never in
	// neither and never ahead of it. At the end every connected counter is sent the closed
	// window and no other widget is; a reload then gets the closed window and no backfill.
	// A wrong token gets 403 and no frame at all.
	const bool tallyRulesOk = BroadcastTallyRules();
	const bool streamStartOk = StreamStartOpensWindow();
	bool tallyLiveOk = false;
	bool tallyTestOk = false;
	bool tallyRaceOk = false;
	bool tallyForcedOk = false;
	bool tallyEndPushOk = false;
	bool tallyEndedOk = false;
	bool tallyAuthOk = false;
	int raceInTally = 0;
	int raceLive = 0;
	{
		Overlay::Widget counter;
		counter.id = "selftest-counter";
		counter.token = "selftesttoken3";
		counter.name = "selftest counter";
		counter.type = "counter";
		Overlay::Store().InjectForTest(counter);

		const std::string kPath = "/w/selftest-counter/events?t=selftesttoken3";
		const std::string since = "\"since\":" + std::to_string(liveSinceMs);
		const auto tallyArrived = [&since](const std::string &a) {
			return NamedFrameArrived(a, "tally", since);
		};
		const auto counted = [&](const std::string &id) {
			Events::NormalizedEvent e;
			e.id = id;
			e.platform = "twitch";
			e.type = "follow";
			e.ts = liveSinceMs + 1;
			e.actorName = id + "-actor";
			return e;
		};

		server.Broadcast(counted("selftest-tally-live"));
		const std::string live = ConnectSse(port, kPath, tallyArrived);
		tallyLiveOk = NamedFrameArrived(live, "tally", since) &&
			      NamedFrameArrived(live, "tally", "\"until\":null") &&
			      NamedFrameArrived(live, "tally", "\"selftest-tally-live\"") &&
			      NamedFrameArrived(live, "tally", "\"type\":\"follow\"") &&
			      live.find("selftest-tally-live-actor") == std::string::npos;

		// A test event and a test stream start, then a reload: neither counted nor moved.
		server.BroadcastTo("selftest-counter", counted("selftest-tally-test"));
		Overlay::json testStart = Overlay::json::object();
		testStart["active"] = true;
		testStart["startedAt"] = liveSinceMs + 5000;
		server.SendTestFrame("selftest-counter", "stream", testStart);
		const std::string afterTest = ConnectSse(port, kPath, tallyArrived);
		tallyTestOk = NamedFrameArrived(afterTest, "tally", since) &&
			      !NamedFrameArrived(afterTest, "tally", "selftest-tally-test") &&
			      !NamedFrameArrived(afterTest, "stream", std::to_string(liveSinceMs + 5000));

		// Connects racing a broadcast. The event's actor name rides only its live frame, so
		// where it sits says which way the race went.
		constexpr int kRaces = 12;
		tallyRaceOk = true;
		for (int i = 0; i < kRaces && tallyRaceOk; ++i) {
			const std::string id = "selftest-race-" + std::to_string(i);
			const std::string actor = id + "-actor";
			SOCKET raceSse = DialLoopback(port);
			if (raceSse == INVALID_SOCKET) {
				tallyRaceOk = false;
				break;
			}
			WriteAll(raceSse, "GET " + kPath + " HTTP/1.1\r\nHost: x\r\n\r\n");
			server.Broadcast(counted(id));
			std::string acc = RecvHeaders(raceSse);
			const DWORD rtoMs = 100;
			setsockopt(raceSse, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rtoMs, sizeof(rtoMs));
			PumpUntil(raceSse, acc, nullptr, [&](const std::string &a) {
				return tallyArrived(a) && (NamedFrameArrived(a, "tally", "\"" + id + "\"") ||
							   a.find(actor) != std::string::npos);
			});
			closesocket(raceSse);
			const size_t tallyAt = acc.find("event: tally");
			const size_t liveAt = acc.find(actor);
			const bool inTally = NamedFrameArrived(acc, "tally", "\"" + id + "\"");
			const bool early = liveAt != std::string::npos &&
					   (tallyAt == std::string::npos || liveAt < tallyAt);
			raceInTally += inTally ? 1 : 0;
			raceLive += liveAt != std::string::npos ? 1 : 0;
			tallyRaceOk = tallyAt != std::string::npos && (inTally || liveAt != std::string::npos) &&
				      !early;
		}

		// The race forced: a broadcast that starts once the socket is registered but before
		// anything is read. Whether the read then counts it or not, it must reach the page,
		// and after the tally -- the send mutex holds it until the replay is out.
		{
			std::mutex forcedMutex;
			std::thread forced;
			server.SetRegisteredObserverForTest([&](const std::string &widgetId) {
				if (widgetId != "selftest-counter") {
					return;
				}
				std::lock_guard<std::mutex> lock(forcedMutex);
				if (!forced.joinable()) {
					forced =
						std::thread([&] { server.Broadcast(counted("selftest-race-forced")); });
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}
			});
			const std::string acc = ConnectSse(port, kPath, [](const std::string &a) {
				return a.find("selftest-race-forced-actor") != std::string::npos;
			});
			server.SetRegisteredObserverForTest(nullptr);
			{
				std::lock_guard<std::mutex> lock(forcedMutex);
				if (forced.joinable()) {
					forced.join();
				}
			}
			const size_t tallyAt = acc.find("event: tally");
			const size_t liveAt = acc.find("selftest-race-forced-actor");
			tallyForcedOk = tallyAt != std::string::npos && liveAt != std::string::npos && liveAt > tallyAt;
		}

		// The end, with a counter and an alert box connected. The fence is a live event
		// sent after the end on the same thread, so a socket that has seen it has been sent
		// everything the end sent.
		SOCKET counterSse = DialLoopback(port);
		SOCKET alertSse = DialLoopback(port);
		if (counterSse != INVALID_SOCKET && alertSse != INVALID_SOCKET) {
			WriteAll(counterSse, "GET " + kPath + " HTTP/1.1\r\nHost: x\r\n\r\n");
			WriteAll(alertSse, "GET /w/selftest-widget/events?t=selftesttoken HTTP/1.1\r\nHost: x\r\n\r\n");
			std::string counterAcc = RecvHeaders(counterSse);
			std::string alertAcc = RecvHeaders(alertSse);
			const DWORD rtoMs = 100;
			setsockopt(counterSse, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rtoMs, sizeof(rtoMs));
			setsockopt(alertSse, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rtoMs, sizeof(rtoMs));
			// What each is sent on connect comes after its registration, so once that is in
			// both are targets of the end.
			PumpUntil(counterSse, counterAcc, nullptr, tallyArrived);
			PumpUntil(alertSse, alertAcc, nullptr, [](const std::string &a) {
				return NamedFrameArrived(a, "channels", "selftest:channels");
			});
			const size_t seeded = counterAcc.size();

			server.BroadcastStreamState(StreamStateJson({}, false, TimeUtil::NowMs()));
			server.Broadcast(counted("selftest-tally-fence"));
			const auto fenced = [](const std::string &a) {
				return a.find("selftest-tally-fence-actor") != std::string::npos;
			};
			const bool counterFenced = PumpUntil(counterSse, counterAcc, nullptr, fenced);
			const bool alertFenced = PumpUntil(alertSse, alertAcc, nullptr, fenced);
			const std::string pushed = counterAcc.substr(seeded);
			tallyEndPushOk = counterFenced && alertFenced && NamedFrameArrived(pushed, "tally", since) &&
					 !NamedFrameArrived(pushed, "tally", "\"until\":null") &&
					 !NamedFrameArrived(pushed, "tally", "selftest-tally-fence") &&
					 alertAcc.find("event: tally") == std::string::npos;
		}
		if (counterSse != INVALID_SOCKET) {
			closesocket(counterSse);
		}
		if (alertSse != INVALID_SOCKET) {
			closesocket(alertSse);
		}

		const std::string after = ConnectSse(port, kPath, tallyArrived);
		tallyEndedOk = NamedFrameArrived(after, "tally", since) &&
			       !NamedFrameArrived(after, "tally", "\"until\":null") &&
			       NamedFrameArrived(after, "tally", "\"selftest-tally-live\"") &&
			       after.find("event: backfill") == std::string::npos;

		SOCKET bad = DialLoopback(port);
		if (bad != INVALID_SOCKET) {
			WriteAll(bad, "GET /w/selftest-counter/events?t=wrong HTTP/1.1\r\nHost: x\r\n\r\n");
			const std::string resp = RecvUntilClose(bad);
			tallyAuthOk = StatusOf(resp) == 403 && resp.find("event: tally") == std::string::npos;
			closesocket(bad);
		}
		Overlay::Store().RemoveForTest("selftest-counter");
	}
	const bool tallyOk = tallyRulesOk && streamStartOk && tallyLiveOk && tallyTestOk && tallyRaceOk &&
			     tallyForcedOk && tallyEndPushOk && tallyEndedOk && tallyAuthOk;
	HostLog(std::string("[selftest] overlay tally over SSE -> ") + (tallyOk ? "OK" : "MISMATCH") +
		" (live=" + (tallyLiveOk ? "ok" : "bad") + " test=" + (tallyTestOk ? "ok" : "bad") +
		" race=" + (tallyRaceOk ? "ok" : "bad") + " [" + std::to_string(raceInTally) + " in tally, " +
		std::to_string(raceLive) + " live] forced=" + (tallyForcedOk ? "ok" : "bad") +
		" endpush=" + (tallyEndPushOk ? "ok" : "bad") + " ended=" + (tallyEndedOk ? "ok" : "bad") +
		" auth=" + (tallyAuthOk ? "ok" : "bad") + ")");

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

	// 7) The per-TYPE replay gate. Two SSE sockets -- one on a type that accepts a replay
	// (alertbox) and one on a type that must never receive one (ticker, which accumulates a
	// belt of recent events, so a replay would put a second copy of a real moment on stream)
	// -- with a single replay broadcast sandwiched between two LIVE ones.
	//
	// The live frames are the harness, not the subject. The first proves both sockets are
	// registered, since RunSse registers only after its headers are on the wire and a
	// broadcast before that would race. The second is a FENCE: frames arrive on one socket
	// in order, so a ticker socket that has seen the fence has provably not been sent the
	// replay that preceded it. That is an assertion about what DID arrive rather than a wait
	// on nothing arriving, so it neither sleeps nor passes by timing out.
	//
	// This is the invariant the acceptsReplay column in overlay_template.cpp encodes, and
	// the one most likely to rot: a thirteenth type added with no row -- or with a row that
	// leaves the column off -- changes what this step sees.
	bool replayGateOk = false;
	{
		Overlay::Widget acc;
		acc.id = "selftest-ticker";
		acc.token = "selftesttoken2";
		acc.name = "selftest accumulator";
		acc.type = "ticker";
		Overlay::Store().InjectForTest(acc);

		SOCKET alertSse = DialLoopback(port);
		SOCKET tickSse = DialLoopback(port);
		if (alertSse != INVALID_SOCKET && tickSse != INVALID_SOCKET) {
			WriteAll(alertSse, "GET /w/selftest-widget/events?t=selftesttoken HTTP/1.1\r\nHost: x\r\n\r\n");
			WriteAll(tickSse, "GET /w/selftest-ticker/events?t=selftesttoken2 HTTP/1.1\r\nHost: x\r\n\r\n");
			std::string alertAcc = RecvHeaders(alertSse);
			std::string tickAcc = RecvHeaders(tickSse);
			const DWORD rtoMs = 100;
			setsockopt(alertSse, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rtoMs, sizeof(rtoMs));
			setsockopt(tickSse, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rtoMs, sizeof(rtoMs));

			// One builder for all three events so the replay differs from the two live
			// frames in exactly the flag under test and its marker, nothing else.
			auto eventNamed = [](const char *marker) {
				Events::NormalizedEvent e;
				e.id = marker;
				e.platform = "twitch";
				e.type = "follow";
				e.ts = 1000;
				e.actorName = "selftest-gate";
				return e;
			};
			auto arrived = [](const char *marker) {
				return [marker](const std::string &a) {
					return a.find(marker) != std::string::npos;
				};
			};
			auto pushLive = [&](const char *marker) {
				return [&, marker] {
					server.Broadcast(eventNamed(marker));
				};
			};

			const bool warmAlert = PumpUntil(alertSse, alertAcc, pushLive("selftest-gate-warm"),
							 arrived("selftest-gate-warm"));
			const bool warmTick = PumpUntil(tickSse, tickAcc, pushLive("selftest-gate-warm"),
							arrived("selftest-gate-warm"));

			const size_t delivered = server.Broadcast(eventNamed("selftest-gate-replay"), /*replay=*/true);

			const bool fenceAlert = PumpUntil(alertSse, alertAcc, pushLive("selftest-gate-fence"),
							  arrived("selftest-gate-fence"));
			const bool fenceTick = PumpUntil(tickSse, tickAcc, pushLive("selftest-gate-fence"),
							 arrived("selftest-gate-fence"));

			const bool alertTook = alertAcc.find("selftest-gate-replay") != std::string::npos &&
					       alertAcc.find("\"replay\":true") != std::string::npos;
			// Both markers, because they fail differently: the id catches a frame that
			// reached the wrong type, and the flag catches one that reached it stripped
			// of the marking that tells a widget what it is looking at.
			const bool tickRefused = tickAcc.find("selftest-gate-replay") == std::string::npos &&
						 tickAcc.find("\"replay\":true") == std::string::npos;
			// Exactly one. The ticker is connected and a filter that let it through would
			// have counted it, so this pins `delivered` to widgets that passed the gate
			// rather than to open sockets.
			replayGateOk = warmAlert && warmTick && fenceAlert && fenceTick && alertTook && tickRefused &&
				       delivered == 1;
		}
		if (alertSse != INVALID_SOCKET) {
			closesocket(alertSse);
		}
		if (tickSse != INVALID_SOCKET) {
			closesocket(tickSse);
		}
		Overlay::Store().RemoveForTest("selftest-ticker");
	}
	HostLog(std::string("[selftest] overlay replay gate -> ") + (replayGateOk ? "OK" : "MISMATCH"));

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

	const bool atomicWriteOk = AtomicWriteSurvivesHeldTarget();

	server.Stop();
	// Leave the shared singleton clean: the real boot Server() must not serve this
	// injected test widget after the smoke run.
	Overlay::Store().RemoveForTest("selftest-widget");
	HostLog("[selftest] overlay cleanup -> server stopped");

	if (docOk && libraryOk && sseHeaderOk && deliveryOk && channelsOk && replayOk && noWindowOk && replayScopeOk &&
	    tallyOk && authOk && replayGateOk && stockOk && sizesOk && atomicWriteOk) {
		HostLog("[selftest] overlay -> document/library/SSE/channels/replay/tally/gate/auth/stock/sizes/atomic-write "
			"OK");
	} else {
		HostLog("[selftest] overlay -> FAILED (see step lines above)");
	}

	WSACleanup();
}
