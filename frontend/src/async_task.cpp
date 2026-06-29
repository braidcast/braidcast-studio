#include "async_task.hpp"

#include <atomic>
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
	std::thread(std::move(work)).detach();
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
