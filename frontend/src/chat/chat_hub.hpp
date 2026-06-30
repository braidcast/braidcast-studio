#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HUB_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HUB_HPP_

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The ChatHub (Phase 9.0): owns the set of live per-platform chat transports
// between go-live and stop. On Start it enumerates connected, scope-current
// accounts, asks each provider for its ChatTransport (chat()), and runs every
// non-null transport on its own detached worker (AsyncTask::RunAsync), fanning
// normalized messages and state to JS via the alive-guarded PostToUi + EmitEvent
// path. Stop signals every loop, disconnects each transport, and clears the set.
//
// Thread-safety: the active-transport map is mutex-guarded. Start/Stop are driven
// from the UI thread (streaming.start/stop + Bridge::Shutdown) so they never run
// concurrently; the detached workers touch the map only under the mutex. Stop is
// idempotent and Start calls it first, so a re-Start never leaves stale workers.
// A function-local-static singleton (Chat::Hub()) so it outlives the detached
// workers to process exit (the workers capture it raw, which is therefore safe).
//
// In Task 2 the foundation ships with NO platform transports: every provider's
// chat() returns null, so Start finds nothing to run and the hub stays idle.
namespace Chat {

using json = nlohmann::json;

class ChatTransport;

class ChatHub {
public:
	// Start a transport per connected, scope-current account. Idempotent: calls
	// Stop() first so a re-Start never leaves stale workers or sockets.
	void Start();

	// Signal every loop to stop, disconnect each transport, clear the set.
	// Idempotent; safe to call when nothing is running, from streaming.stop and
	// from Bridge::Shutdown.
	void Stop();

	// Route `text` to each active transport whose providerId is in `platforms`
	// (empty = all connected). Each send runs on its own worker so a slow REST send
	// never blocks the caller; a failure emits a chat.state error for that platform.
	void SendToPlatforms(const std::vector<std::string> &platforms, const std::string &text);

	// Per-active-transport status: [{ platform, connected, error }].
	json State();

private:
	struct Active {
		std::string providerId;
		ChatTransport *transport = nullptr; // owned by the provider (lives to exit)
		bool connected = false;
		std::string error;
	};

	std::mutex mutex_;
	std::map<std::string, Active> active_;    // keyed by profileUuid
	std::shared_ptr<std::atomic<bool>> stop_; // current generation's cancel flag
};

// Process-wide chat hub accessor (function-local-static singleton, mirroring the
// other frontend stores' free-accessor shape).
ChatHub &Hub();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HUB_HPP_
