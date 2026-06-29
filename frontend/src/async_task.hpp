#ifndef OBS_MULTISTREAM_FRONTEND_ASYNC_TASK_HPP_
#define OBS_MULTISTREAM_FRONTEND_ASYNC_TASK_HPP_

#include <functional>

// Off-thread work + UI-thread marshal helpers for the bridge (Phase 8a). Lets
// OAuth/platform-API code run blocking HTTP on a worker thread and then emit
// bridge events safely back on the CEF UI thread.
namespace AsyncTask {

// Run `work` on a detached background thread. `work` MUST own everything it
// touches (no captured raw pointers that may die before it runs) -- the thread
// can outlive the caller. To report back, call PostToUi from inside `work`.
void RunAsync(std::function<void()> work);

// Marshal `fn` onto the CEF UI thread (mirrors the bridge's EmitEvent path:
// CefPostTask(TID_UI, ...), or runs inline when already on TID_UI). If the
// bridge has been torn down (see SetAlive), `fn` is dropped and never runs, so
// a late callback from a detached worker can't touch CEF after shutdown.
void PostToUi(std::function<void()> fn);

// Toggle the alive-guard. Called with false during bridge teardown (on the UI
// thread) so any in-flight PostToUi no-ops thereafter.
void SetAlive(bool alive);

} // namespace AsyncTask

#endif // OBS_MULTISTREAM_FRONTEND_ASYNC_TASK_HPP_
