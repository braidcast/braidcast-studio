#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HUB_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HUB_HPP_

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The ChatHub (Phase 9.0): owns the set of live per-account chat transports between
// go-live and stop. On Start it enumerates connected, scope-current accounts targeted
// by an ENABLED output binding, builds a fresh transport per account via
// provider->makeChat(acct), and runs every non-null transport on its own detached
// worker (AsyncTask::RunAsync), fanning normalized
// messages and state to JS via the alive-guarded PostToUi + EmitEvent path. Each
// transport is hub-owned as a shared_ptr shared with its worker, so Stop (which drops
// the hub's ref, signals every loop, and disconnects each transport) can't free a
// transport out from under an in-flight connect(); the object survives until the
// worker's captured copy also drops.
//
// Thread-safety: the active-transport map is mutex-guarded. Start/Stop are driven
// from the UI thread (streaming.start/stop + Bridge::Shutdown) so they never run
// concurrently; the detached workers touch the map only under the mutex. Stop is
// idempotent and Start calls it first, so a re-Start never leaves stale workers.
// A function-local-static singleton (Chat::Hub()) so it outlives the detached
// workers to process exit (the workers capture it raw, which is therefore safe).
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
		std::shared_ptr<ChatTransport> transport; // hub-owned, shared with the worker
		bool connected = false;
		std::string error;
	};

	std::mutex mutex_;
	std::map<std::string, Active> active_;    // keyed by accountId
	std::shared_ptr<std::atomic<bool>> stop_; // current generation's cancel flag

	// Monotonic sequence for synthesizing unique fallback message ids when a
	// transport emits a chat.message with an empty/missing id (Kick's payload may
	// lack one; Twitch's id tag is "" if tags are dropped). Guarantees the keyed
	// frontend list never sees a duplicate/empty key.
	std::atomic<uint64_t> idSeq_{0};
};

// Process-wide chat hub accessor (function-local-static singleton, mirroring the
// other frontend stores' free-accessor shape).
ChatHub &Hub();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HUB_HPP_
