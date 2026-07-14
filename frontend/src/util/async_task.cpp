#include "async_task.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"

namespace AsyncTask {

namespace {

// Cleared to false at bridge teardown so a PostToUi queued by a still-running
// detached worker is dropped instead of touching CEF after the loop is gone.
// Starts true: the bridge is alive for the whole window before Shutdown().
std::atomic<bool> g_alive{true};

// Count of live RunAsync workers, so shutdown can wait for them to unwind before
// tearing down the hubs/statics they may still be mid-call on. The workers stay
// detached (no join handles kept); this counter + condvar just let one waiter
// block until the count reaches zero.
std::mutex g_workerMutex;
std::condition_variable g_workerCv;
int g_workerCount = 0;

// base::BindOnce in this codebase binds free functions; route the std::function
// through this trampoline so the closure machinery has a plain target.
void InvokeOnUi(std::function<void()> fn)
{
	if (!g_alive.load(std::memory_order_acquire)) {
		return;
	}
	fn();
}

} // namespace

void RunAsync(std::function<void()> work)
{
	{
		std::lock_guard<std::mutex> lock(g_workerMutex);
		++g_workerCount;
	}
	try {
		std::thread([work = std::move(work)]() mutable {
			try {
				work();
			} catch (...) {
				// Never let an escaped exception skip the count decrement below
				// (it would wedge WaitForDrain forever). RunOAuthConnect and
				// RunAsyncMethod already wrap their own bodies; this is belt-and-braces.
			}
			{
				std::lock_guard<std::mutex> lock(g_workerMutex);
				--g_workerCount;
			}
			g_workerCv.notify_all();
		}).detach();
	} catch (...) {
		// The std::thread ctor threw (thread/handle exhaustion) before the worker
		// could run, so the decrement above never fires -- undo the reserved count
		// here or WaitForDrain wedges the full timeout at shutdown. Rethrow so the
		// caller resolves its callback with a failure instead of hanging.
		{
			std::lock_guard<std::mutex> lock(g_workerMutex);
			--g_workerCount;
		}
		g_workerCv.notify_all();
		throw;
	}
}

bool WaitForDrain(std::chrono::milliseconds timeout)
{
	std::unique_lock<std::mutex> lock(g_workerMutex);
	return g_workerCv.wait_for(lock, timeout, [] { return g_workerCount == 0; });
}

void PostToUi(std::function<void()> fn)
{
	if (!g_alive.load(std::memory_order_acquire)) {
		return;
	}
	if (CefCurrentlyOn(TID_UI)) {
		fn();
		return;
	}
	CefPostTask(TID_UI, base::BindOnce(&InvokeOnUi, std::move(fn)));
}

void SetAlive(bool alive)
{
	g_alive.store(alive, std::memory_order_release);
}

} // namespace AsyncTask
