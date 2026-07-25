#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HUB_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HUB_HPP_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../oauth/provider.hpp" // OAuth::DestinationId

// The ChatHub (Phase 9.0): owns the set of live per-destination chat transports between
// go-live and stop. On Start it enumerates connected, scope-current accounts targeted
// by an ENABLED output binding, builds a fresh transport per destination via
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
struct OutputBinding;

namespace Chat {

using json = nlohmann::json;

class ChatTransport;

// The live destination one output binding streams to, false when its stream profile has no
// linked, still-known account. THE DESTINATION KEYING RULE lives here, in one place, so
// every subsystem that has a binding and needs its destination agrees:
//   - StreamProvider::broadcastPerDestination() == false (Twitch, Kick): the platform has
//     exactly ONE chat and ONE viewer figure per account, so every profile pointing at that
//     account normalizes to the account-wide destination (empty profileUuid). Two profiles on
//     one Twitch channel therefore still yield ONE transport and ONE IRC connection; a second
//     would double every message and every send.
//   - broadcastPerDestination() == true (YouTube): each profile's broadcast has its own
//     liveChatId and its own viewer count, so each profile is its own destination. Collapsing
//     them is what left one orientation's chat permanently unread.
// Says nothing about whether the binding is ENABLED or live -- callers decide that, because
// the teardown path runs precisely when a binding has just been disabled or is about to be
// removed.
bool BindingDestination(const OutputBinding &b, OAuth::DestinationId &out);

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
	// Platform-wide: on a platform with one broadcast per destination this posts to
	// EVERY live destination of that platform, which is why a REPLY must use
	// SendToDestination below instead.
	void SendToPlatforms(const std::vector<std::string> &platforms, const std::string &text);

	// Route `text` to ONE destination's transport, so a caller that knows which broadcast
	// it is replying to reaches exactly that chat instead of every chat on the platform.
	// An account-wide destination (empty profileUuid) matches that account's single
	// transport on a per-channel platform. Same worker/echo/error path as
	// SendToPlatforms -- both dispatch through one implementation.
	//
	// Returns false when no ACTIVE transport holds that exact destination, sending
	// nothing: a targeted reply whose target is gone must surface as a failure, never
	// fall back to the platform (that is the fan-out it exists to avoid) and never
	// silently drop.
	bool SendToDestination(const OAuth::DestinationId &dest, const std::string &text);

	// Per-active-transport status: [{ platform, accountId, profileUuid, connected, error }].
	json State();

private:
	struct Active {
		std::string providerId;
		OAuth::DestinationId dest;
		std::shared_ptr<ChatTransport> transport; // hub-owned, shared with the worker
		// The destination's transport emit path (the same closure handed to the worker as
		// ctx.emit: stop-guard -> identity stamp -> fallback-id synthesis -> overlay
		// fan-out -> alive-guarded UI post). Stored so a send can route the local echo of
		// an outbound message through the identical path a real incoming message takes.
		std::function<void(const json &payload)> emit;
		bool connected = false;
		std::string error;
	};

	// The ONE per-transport send body both public entry points reach: one worker per
	// target (a slow REST send must never block the caller), the chat.state error emit
	// on failure, and the local echo for a platform whose read path never reflects the
	// sender's own line. Called with the mutex RELEASED, on a copy of the Active row.
	void DispatchSend(const Active &target, const std::string &text);

	std::mutex mutex_;
	// Keyed by DESTINATION, not accountId. For a platform whose chat is per-account
	// (Twitch/Kick) every profile collapses onto one account-wide destination, so the map
	// holds exactly one entry per account exactly as before; for YouTube each live
	// broadcast gets its own entry and therefore its own transport. See
	// StreamProvider::broadcastPerDestination for the rule and EnabledChatDestinations for
	// where it is applied.
	std::map<OAuth::DestinationId, Active> active_;
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
