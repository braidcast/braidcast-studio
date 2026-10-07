#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SERVER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SERVER_HPP_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "../events/event_model.hpp" // Events::NormalizedEvent
#include "broadcast_tally.hpp"

namespace Overlay {

// The request headers a route may act on, read once in HandleConnection -- the only scope
// that still holds the header block -- and handed to every route through the shared handler
// signature, so the route table stays one data list. Empty when the client did not send one.
struct RequestHeaders {
	std::string ifNoneMatch; // If-None-Match, verbatim
	std::string range;       // Range, verbatim
	std::string ifRange;     // If-Range, verbatim
};

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

	// Stop the fan-out taking frames, close the listen socket, shut down every client socket
	// (unblocking their recv loops and any send in flight), then join the fan-out -- frames
	// still queued are counted, not sent, and resolve to 0 -- flush the tally, and join all
	// connection threads. Called from Bridge::Shutdown before CEF teardown.
	void Stop();

	bool IsListening() const { return running_.load(); }
	int Port() const { return port_; }
	bool PortChanged() const { return portChanged_; }
	std::string LastError() const { return lastError_; }

	// Every frame below goes out through ONE queue and ONE fan-out thread (FanoutLoop), in
	// the order it was queued. A producer only queues, which is O(1) under its own locks, so
	// neither the event hub's admission lock nor a chat read worker waits on a widget socket;
	// and frames queued in an order on one thread (a removal's frames after the events they
	// reach, a stream end before the event that fences it) reach every socket in that order.
	// The calls that return a count wait for their own frame to be sent, so they must not be
	// made under a lock the fan-out thread needs to send: sseMutex_, any socket's send mutex,
	// the widget store's mutex, the tally's, clientsMutex_ or the log.

	// Push a live NormalizedEvent (the EventHub::Ingest sink) to EVERY open widget socket.
	// The broadcast tally counts it on the fan-out thread, right before its frame is sent.
	void Broadcast(const Events::NormalizedEvent &ev);
	// Push a stored event again (events.replay -- never counted) to the sockets of widgets
	// whose TYPE accepts a replay (Overlay::AcceptsReplay). Resolves, once it is sent, to how
	// many WIDGETS took it -- not sockets, so one widget open in both the editor preview and
	// a Browser Source counts once -- so a replay can report "nothing received it" instead of
	// claiming a delivery it cannot see. 0 when the server is not running.
	std::future<size_t> Replay(const Events::NormalizedEvent &ev);
	// Push to ONE widget's sockets (overlays.test -- never goes through the store or the
	// tally), marked `"test": true` so a page can keep it out of anything it keeps.
	// Returns how many widgets took it (0 or 1), so a test can report that nothing was
	// listening rather than claim a delivery it cannot see. Waits for the send.
	size_t BroadcastTo(const std::string &widgetId, const Events::NormalizedEvent &ev);
	// Push a chat message to EVERY open widget socket as a named `chat` SSE event
	// (distinct from the default `message` event alert boxes consume). The chat-box
	// widget subscribes to it; alert boxes ignore it.
	void BroadcastChat(const nlohmann::json &chatMsg);
	// Push a moderator's removal to EVERY open widget socket as a named `moderation` SSE
	// event, so a widget showing chat drops the lines it names. The body names lines by id
	// and author id only, never by text (Chat::OverlayModerationBody). Like `chat`, it is
	// never kept for replay: a widget that connects later never had the lines.
	void BroadcastChatModeration(const nlohmann::json &op);
	// Push the ids of stored events a moderator's removal took the viewer's words from
	// (`{"ids": [...]}`, never text) to EVERY open widget socket as a named `eventredaction`
	// SSE event, so a widget still holding one of those events drops its message. Never kept
	// for replay: the backfill and events.replay read the already-redacted store.
	void BroadcastEventRedaction(const nlohmann::json &ids);
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
	// named `stream` SSE event. Also what moves the broadcast tally's latched window
	// (BroadcastTally::OnStreamState), which the backfill reads too; when it moves, every
	// widget that counts events is sent the new `tally` right after this frame. Replayed on
	// connect (see replayFrames_), so a browser
	// source added mid-broadcast learns the state at once instead of at the next
	// transition. It is also the only closing signal an overlay gets: the viewer poller
	// stops with the stream without pushing a final zero, so a viewer widget clears off
	// `active` going false rather than inventing a 0 of its own.
	void BroadcastStreamState(const nlohmann::json &state);
	// Send a named-channel frame to ONE widget, bypassing the replay cache and the tally
	// window: a preview test must never become the state a real browser source replays on
	// connect. Mirrors BroadcastTo's "never the store" rule for the default channel, marks an
	// object body `"test": true` the same way, and reports the same delivery count.
	size_t SendTestFrame(const std::string &widgetId, const char *eventName, const nlohmann::json &body);
	// Save the broadcast tally if it holds counts its event path has not saved yet
	// (BroadcastTally::SaveIfDue). The bridge's 1 Hz stats tick calls it, on TID_UI.
	void SaveTallyIfDue();

	// Self-tests only: called on a connecting SSE socket's thread right after it registers,
	// while it still holds the socket's send mutex and has read nothing it replays -- the
	// moment a broadcast racing the connect is decided. Null clears it. The fan-out's next
	// frame to that widget waits for that send mutex, and every later frame waits behind it,
	// so the observer must not wait for a send (BroadcastTo, SendTestFrame, Replay's future,
	// DrainForTest); queueing one (Broadcast) is fine.
	void SetRegisteredObserverForTest(std::function<void(const std::string &widgetId)> observer);
	// Self-tests only: return once every frame queued before the call has been sent, for a
	// step that needs a broadcast to be out before it connects.
	void DrainForTest();

private:
	void AcceptLoop();
	void HandleConnection(uintptr_t clientSocket); // runs on its own thread; closes the socket
	// Route handlers. Each owns closing the socket.
	void ServeRuntime(uintptr_t sock, const std::string &path, const std::string &token,
			  const RequestHeaders &headers);
	void ServeWidget(uintptr_t sock, const std::string &path, const std::string &token,
			 const RequestHeaders &headers);
	// GET /lib/<path>: the bundled sound library. No token -- these are public files from
	// the app's own bundle -- and nothing outside LibraryRoot() (IsSafeLibraryPath).
	void ServeLibrary(uintptr_t sock, const std::string &path, const std::string &token,
			  const RequestHeaders &headers);
	// Send a prebuilt SSE frame to every open widget socket, or (with onlyWidgetId set)
	// to one widget's sockets only, or (with widgetFilter set) to only the widgets it
	// answers true for -- events.replay's per-type gate; the two selectors are never
	// combined by a real caller. With keepAs set the frame also becomes that state channel's
	// replay copy (replayFrames_), written in the same sseMutex_ section that snapshots the
	// sockets, so a connecting page gets the frame exactly once: from its replay if it
	// registered before that section, live if after. The single snapshot-under-lock /
	// send-unlocked implementation every frame goes through, so sseMutex_ is never held
	// across the bounded-blocking sends -- nor across widgetFilter, which reads the widget
	// store, so a connection registering or tearing down never waits on an overlay save.
	// The fan-out itself does: a save holds the store's mutex across its disk write, which
	// delays this frame, and every frame queued behind it, by up to that write's length.
	// Returns how many WIDGETS took the frame (a widget with two open sockets counts once).
	// Runs on the fan-out thread only (FanoutLoop); everything else queues (Enqueue).
	size_t BroadcastFrame(const std::string &frame, const std::string *onlyWidgetId = nullptr,
			      bool (*widgetFilter)(const std::string &) = nullptr, const std::string *keepAs = nullptr);

	// One queued frame, who it goes to and what is recorded as it goes (BroadcastFrame's
	// parameters). `beforeSend` runs on the fan-out thread right before the frame is sent, so
	// what it records happens in send order: the tally's count of a live event, a stream
	// frame's move of the tally's window. It runs even when the frame is never sent -- a
	// frame left queued at Stop has it run by JoinFanout -- and a failure in it does not stop
	// the send. `onSent` runs on the fan-out thread with the count once it is sent;
	// `delivered` is set then too, for a caller that waits on it.
	struct FrameJob {
		std::string frame;
		std::optional<std::string> onlyWidgetId;
		bool (*widgetFilter)(const std::string &) = nullptr;
		std::string keepAs; // a state channel's name, or empty
		std::function<void()> beforeSend;
		std::function<void(size_t delivered)> onSent;
		std::shared_ptr<std::promise<size_t>> delivered;
	};
	// Queue `job` for the fan-out thread. Every frame is queued, however long the queue: a
	// socket that stalls costs one SO_SNDTIMEO and is dropped, after which the queue drains,
	// and a dropped frame could be one a page's state rests on -- a removal, a stream end, a
	// tally, an event the tally counts. With `wantCount` the returned future resolves to the
	// delivered count; without it the future is empty. A job queued while the fan-out is not
	// running is dropped whole (its beforeSend and keepAs never run) and resolves to 0 at
	// once, as BroadcastFrame would with no sockets.
	std::future<size_t> Enqueue(FrameJob job, bool wantCount = false);
	void FanoutLoop();
	void StartFanout();
	// Tells the fan-out thread to stop after the frame it is sending; nothing queued from
	// then on is sent.
	void SignalFanoutStop();
	// Joins the fan-out thread (SignalFanoutStop first). Frames still queued are not sent, but
	// their beforeSend runs here, in queue order, so the tally counts every event queued
	// before the stop; then they resolve to 0.
	void JoinFanout();
	// A queue this deep is logged once, and again only after it has drained below half: a
	// reader that takes each frame just inside SO_SNDTIMEO is never dropped, but it holds up
	// every widget behind it, and the depth is the only trace of that. Nothing is dropped.
	static constexpr size_t kFanoutBacklogLogDepth = 1024;
	// Guards fanoutQueue_, fanoutRunning_ and fanoutBacklogLogged_; a leaf, never held across a send.
	std::mutex fanoutMutex_;
	std::condition_variable fanoutCv_;
	std::deque<FrameJob> fanoutQueue_;
	bool fanoutRunning_ = false;
	bool fanoutBacklogLogged_ = false;
	std::thread fanoutThread_; // started under fanoutMutex_ (StartFanout), joined by JoinFanout
	// The job for a channel whose latest frame is also KEPT for replay on connect, keyed by
	// eventName. The one place a replayable frame is built, so a second such channel cannot
	// drift from the first; the fan-out stores it as it sends it (BroadcastFrame's keepAs).
	static FrameJob StateFrameJob(const char *eventName, const nlohmann::json &body);
	// Owns the socket for its lifetime. `tally`: the widget counts events, so it is sent the
	// `tally` frame on connect (Overlay::CountsEvents, decided by the caller, which already
	// holds the widget). Registers the socket BEFORE reading anything it replays, holding the
	// socket's send mutex until the replay is out: whatever is broadcast after the read is
	// then delivered, after the replay, and whatever both carry the page dedupes.
	void RunSse(uintptr_t sock, const std::string &widgetId, bool tally);
	// Live SSE sockets across every widget: the capacity ceiling's live half, and the
	// audience a broadcast reaches. Caller must hold sseMutex_.
	size_t LiveSseCount() const;
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

	// One writer at a time per SSE socket. Every byte sent on a registered socket -- a
	// broadcast frame, the replay on connect, the keepalive -- is sent holding that socket's
	// send mutex, so two threads' frames can never interleave on the wire. Before
	// registration only the owning RunSse thread can reach the socket. Lock order: a send
	// mutex is taken with no other server lock held, except by RunSse, which holds its own
	// across sseMutex_, the event store and the tally while it builds the replay -- none of
	// which is ever held while waiting for a send mutex.
	using SendMutex = std::shared_ptr<std::mutex>;

	std::mutex sseMutex_;                                           // guards sockets_ + the fields below
	std::map<std::string, std::map<uintptr_t, SendMutex>> sockets_; // widgetId -> SSE socket -> its send mutex

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
	// a widget does not wait out the interval to the next one. Written only by the fan-out
	// thread, as it sends the frame (BroadcastFrame's keepAs); guarded by sseMutex_ but
	// never sent while holding it. Only channels carrying STATE belong here: `channels`
	// (a ~15 minute poll cadence) and `stream` (changes only at a transition, which may
	// be hours away). Chat and viewers are deliberately absent -- see RunSse.
	std::map<std::string, std::string> replayFrames_;

	// The current or most recent broadcast's event totals and its latched window. Its open
	// window also bounds the `backfill` frame. In memory until Start opens its file, so a
	// self-test server never touches the user's. Counted into and its window moved only on the
	// fan-out thread, each right before the frame it belongs to is sent (Broadcast,
	// BroadcastStreamState) -- or, for a frame still queued at Stop, on the stopping thread
	// once the fan-out is joined (JoinFanout) -- so its counts follow queue order; a save
	// it makes there delays the fan-out by that write. Its own mutex is a leaf: the only server
	// lock it is taken under is a connecting socket's send mutex (RunSse), and nothing waits on
	// another lock while holding it.
	BroadcastTally tally_;

	std::function<void(const std::string &)> registeredObserver_; // guarded by sseMutex_; self-tests only

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
