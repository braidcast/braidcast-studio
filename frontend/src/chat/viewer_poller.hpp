#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_VIEWER_POLLER_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_VIEWER_POLLER_HPP_

#include <chrono>
#include <optional>

#include "account_poller.hpp"

// The ViewerPoller (Phase 9.0): a single background worker that, while live,
// polls each connected, scope-current account's platform viewer count on a modest
// interval, aggregates them into { perAccount: {<accountId>: n}, total }, and
// emits the `viewers.changed` bridge event. The per-platform call sits behind
// StreamProvider::viewerCount so the poller has no per-platform branching; a
// platform reporting "not live / unsupported" (viewerCount returns false) is
// omitted from the aggregate.
//
// Lifecycle mirrors the ChatHub: Start() on streaming.start, Stop() on
// streaming.stop and Bridge::Shutdown (before the alive-guard clears). See
// AccountPoller for the shared idempotent-Start / detached-worker /
// alive-guarded-emit contract.
namespace Chat {

class ViewerPoller : public AccountPoller {
protected:
	const char *LogTag() const override;
	const char *EventName() const override;
	std::chrono::milliseconds Interval(unsigned long long tick) const override;
	void PollAccount(OAuth::OAuthAccount &acct, OAuth::StreamProvider *provider, json &perAccount) override;
	std::optional<json> BuildPayload(json &&perAccount) override;
};

// Process-wide viewer poller accessor (function-local-static singleton, mirroring
// the ChatHub's Chat::Hub() shape so it outlives the detached worker to exit).
ViewerPoller &Viewers();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_VIEWER_POLLER_HPP_
