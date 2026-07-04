#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_VIEWER_POLLER_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_VIEWER_POLLER_HPP_

#include <atomic>
#include <memory>
#include <mutex>

// The ViewerPoller (Phase 9.0): a single background worker that, while live,
// polls each connected, scope-current account's platform viewer count on a modest
// interval, aggregates them into { perAccount: {<accountId>: n}, total }, and
// emits the `viewers.changed` bridge event. The per-platform call sits behind
// StreamProvider::viewerCount so the poller has no per-platform branching; a
// platform reporting "not live / unsupported" (viewerCount returns false) is
// omitted from the aggregate.
//
// Lifecycle mirrors the ChatHub: Start() on streaming.start, Stop() on
// streaming.stop and Bridge::Shutdown (before the alive-guard clears). Start is
// idempotent (calls Stop() first). The worker is detached; Stop() signals its
// cancel flag and the worker exits within ~0.5s (a sliced CancelableSleep + a
// canceled() check between platform calls). All emits go through the alive-guarded
// AsyncTask::PostToUi + Bridge::EmitEvent path, so a late emit after Shutdown is
// dropped rather than touching CEF.
namespace Chat {

class ViewerPoller {
public:
	// Spawn the single poll worker. Idempotent: calls Stop() first so a re-Start
	// never leaves a stale worker running.
	void Start();

	// Signal the worker to stop. Idempotent; safe when nothing is running, from
	// streaming.stop and Bridge::Shutdown.
	void Stop();

private:
	std::mutex mutex_;
	std::shared_ptr<std::atomic<bool>> stop_; // current generation's cancel flag
};

// Process-wide viewer poller accessor (function-local-static singleton, mirroring
// the ChatHub's Chat::Hub() shape so it outlives the detached worker to exit).
ViewerPoller &Viewers();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_VIEWER_POLLER_HPP_
