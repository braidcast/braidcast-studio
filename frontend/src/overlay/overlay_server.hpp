#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SERVER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SERVER_HPP_

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "../events/event_model.hpp" // Events::NormalizedEvent

namespace Overlay {

// Loopback-only HTTP/1.1 server for overlay widgets. GET routing + static file
// serving + long-lived SSE. 127.0.0.1 only; per-widget token on every route.
// Distinct from mcp/HttpServer (which is POST-only, single-connection).
class OverlayServer {
public:
	OverlayServer() = default;
	~OverlayServer();
	OverlayServer(const OverlayServer &) = delete;
	OverlayServer &operator=(const OverlayServer &) = delete;

	// Bind 127.0.0.1: try the store's persisted port, else scan five scattered bands of
	// 50 (43000/47000/51000/55000/59000) for a free one; persist the bound port via
	// Store().SetPort. Spawns the accept thread. Idempotent. Returns false (logged) if
	// no port in any band binds.
	bool Start();

	// Test entry point: bind an explicit port (0 => OS-ephemeral) WITHOUT touching
	// the store's persisted port; returns the actually-bound port via *boundPort.
	bool StartForTest(int port, int *boundPort);

	// Close the listen socket + every SSE socket (unblocking their recv loops), then
	// join all connection threads. Called from Bridge::Shutdown before CEF teardown.
	void Stop();

	bool IsListening() const { return running_.load(); }
	int Port() const { return port_; }
	bool PortChanged() const { return portChanged_; }
	std::string LastError() const { return lastError_; }

	// Push a NormalizedEvent to EVERY open widget socket (the EventHub::Ingest sink).
	void Broadcast(const Events::NormalizedEvent &ev);
	// Push to ONE widget's sockets (overlays.test -- never goes through the store).
	// Returns how many took it, so a test can report that nothing was listening rather
	// than claim a delivery it cannot see.
	size_t BroadcastTo(const std::string &widgetId, const Events::NormalizedEvent &ev);
	// Push a chat message to EVERY open widget socket as a named `chat` SSE event
	// (distinct from the default `message` event alert boxes consume). The chat-box
	// widget subscribes to it; alert boxes ignore it.
	void BroadcastChat(const nlohmann::json &chatMsg);
	// Push the poller's concurrent-viewer payload to EVERY open widget socket as a named
	// `viewers` SSE event, forwarded verbatim (nulls and absent rows included -- a
	// destination that never answered is not a zero). Viewer-count widgets subscribe to
	// it; every other widget ignores it.
	void BroadcastViewers(const nlohmann::json &viewers);
	// Push the poller's audience-total payload (followers/subscribers per account) to EVERY
	// open widget socket as a named `channels` SSE event, forwarded verbatim: an absent
	// account was never read, `audienceHidden` means the platform is withholding the number,
	// and a count of -1 means unknown -- none of which is a zero.
	void BroadcastChannelStats(const nlohmann::json &stats);
	// Push broadcast state -- whether anything is live, the wall-clock epoch ms it went
	// live, and the destinations it is going out to -- to EVERY open widget socket as a
	// named `stream` SSE event. Replayed on connect (see replayFrames_), so a browser
	// source added mid-broadcast learns the state at once instead of at the next
	// transition. It is also the only closing signal an overlay gets: the viewer poller
	// stops with the stream without pushing a final zero, so a viewer widget clears off
	// `active` going false rather than inventing a 0 of its own.
	void BroadcastStreamState(const nlohmann::json &state);
	// Send a named-channel frame to ONE widget, bypassing the replay cache and the backfill
	// window: a preview test must never become the state a real browser source replays on
	// connect. Mirrors BroadcastTo's "never the store" rule for the default channel, and
	// reports the same delivery count.
	size_t SendTestFrame(const std::string &widgetId, const char *eventName, const nlohmann::json &body);

private:
	void AcceptLoop();
	void HandleConnection(uintptr_t clientSocket); // runs on its own thread; closes the socket
	void ServeRuntime(uintptr_t sock, const std::string &path, const std::string &token);
	void ServeWidget(uintptr_t sock, const std::string &path, const std::string &token);
	// Send a prebuilt SSE frame to every open widget socket, or (with onlyWidgetId set)
	// to one widget's sockets only. The single snapshot-under-lock / send-unlocked
	// implementation shared by Broadcast/BroadcastChat/BroadcastViewers/
	// BroadcastChannelStats/BroadcastStreamState/BroadcastTo/SendTestFrame, so sseMutex_ is
	// never held across the bounded-blocking sends. Returns how many sockets took the frame.
	size_t BroadcastFrame(const std::string &frame, const std::string *onlyWidgetId = nullptr);
	// BroadcastFrame for a channel whose latest frame is also KEPT for replay on
	// connect, keyed by eventName. The one place a replayable frame is built and
	// stored, so a second such channel cannot drift from the first.
	void BroadcastStateFrame(const char *eventName, const nlohmann::json &body);
	void RunSse(uintptr_t sock, const std::string &widgetId); // owns the socket for its lifetime
	void CloseClient(uintptr_t sock); // the OWNING thread's sole close point: erase from clientSockets_ + close
	void ReapFinishedThreads();       // join+erase threads whose done flag is set

	bool BindOn(int port); // low-level bind+listen helper; sets listenSocket_ + port_

	std::atomic<bool> running_{false};
	std::thread acceptThread_;
	uintptr_t listenSocket_ = ~uintptr_t(0);
	bool wsaUp_ = false;
	int port_ = 0;
	bool portChanged_ = false;
	std::string lastError_;

	std::mutex sseMutex_;                                // guards sockets_ + the fields below
	std::map<std::string, std::set<uintptr_t>> sockets_; // widgetId -> SSE sockets

	// SSE connections mid-handshake: RunSse reserves a capacity slot under sseMutex_,
	// then sends the HTTP header WITHOUT holding the lock (a blocking send must never
	// hold sseMutex_), and only registers into sockets_ once the header is on the wire
	// (a broadcast frame must never precede it).
	size_t ssePending_ = 0;

	// BroadcastFrame snapshots handles then sends OUTSIDE sseMutex_ so one slow client
	// can't stall the others; broadcastDepth_ counts those in-flight sends. While it is
	// non-zero an owning RunSse thread that tears down must NOT closesocket() its fd (a
	// concurrent send could then land on a recycled fd) -- it parks the fd in
	// deferredCloseSse_ instead, and the last broadcast to finish closes them.
	int broadcastDepth_ = 0;
	std::set<uintptr_t> deferredCloseSse_;

	// eventName -> that channel's last frame, replayed to a newly connected SSE client so
	// a widget does not wait out the interval to the next one. Guarded by sseMutex_ but
	// never sent while holding it. Only channels carrying STATE belong here: `channels`
	// (a ~15 minute poll cadence) and `stream` (changes only at a transition, which may
	// be hours away). Chat and viewers are deliberately absent -- see RunSse.
	std::map<std::string, std::string> replayFrames_;

	// When the current broadcast went live, in wall-clock epoch ms, or 0 when nothing is
	// live or no output has reported a start yet. Taken from the `stream` state frame,
	// which is the only place that time is known. It bounds the `backfill` frame RunSse
	// builds on connect: without it there is no window, so no backfill is sent.
	int64_t streamStartedAtMs_ = 0;

	// Every accepted client fd (SSE and plain), so Stop() can shutdown() them all to
	// unblock parked recv/send loops without closing (the owning thread closes). The
	// fd is inserted synchronously in AcceptLoop (before its thread spawns) so a Stop()
	// after the accept thread joins sees every live connection.
	std::mutex clientsMutex_;
	std::set<uintptr_t> clientSockets_;

	struct Conn {
		std::thread thread;
		std::shared_ptr<std::atomic<bool>> done;
	};
	std::mutex threadsMutex_;
	std::vector<Conn> threads_;
};

} // namespace Overlay

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SERVER_HPP_
