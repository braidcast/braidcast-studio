#ifndef OBS_MULTISTREAM_FRONTEND_ASYNC_TASK_HPP_
#define OBS_MULTISTREAM_FRONTEND_ASYNC_TASK_HPP_

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>

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

// Run `fn` on the UI thread and block the calling thread until it returns a value,
// or `timeout` elapses. nullopt means no value arrived in time -- either PostToUi
// dropped the task (bridge torn down) or the UI thread was too busy to reach it.
// Those are not distinguishable here, and in the second case `fn` may still run
// afterwards and take effect, so nullopt is "no answer", never "nothing happened".
//
// This function keeps its own shared_ptr to the promise for the whole wait, which
// is what makes the timeout well-defined: the posted lambda holds a second one, so
// dropping the task destroys neither the promise nor the shared state, and the
// waiter sees a clean timeout rather than a broken_promise. Capturing by reference
// instead would turn a late task into a write to a destroyed object.
//
// `fn` must not throw: an exception escapes into the UI thread's task runner
// instead of reaching the caller, and this call then blocks for the full timeout.
template<typename T> std::optional<T> CallOnUiWithTimeout(std::function<T()> fn, std::chrono::milliseconds timeout)
{
	auto done = std::make_shared<std::promise<T>>();
	std::future<T> fut = done->get_future();

	PostToUi([done, fn = std::move(fn)] { done->set_value(fn()); });

	if (fut.wait_for(timeout) != std::future_status::ready) {
		return std::nullopt;
	}
	return fut.get();
}

// Block until every live RunAsync worker has returned, or `timeout` elapses.
// Called once during bridge teardown BEFORE the hubs/statics those workers may
// touch are torn down, so a detached worker can't resurrect a stopped hub or
// dereference a freed static mid-shutdown. Returns true if all workers drained,
// false if the timeout was hit (the remaining count is logged by the caller).
// Detached-thread semantics stay for the normal path; only shutdown waits.
bool WaitForDrain(std::chrono::milliseconds timeout);

} // namespace AsyncTask

#endif // OBS_MULTISTREAM_FRONTEND_ASYNC_TASK_HPP_
