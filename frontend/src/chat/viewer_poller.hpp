#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_VIEWER_POLLER_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_VIEWER_POLLER_HPP_

#include <chrono>
#include <optional>

#include "account_poller.hpp"

// The ViewerPoller (Phase 9.0): a single background worker that, while live, polls each
// connected, scope-current account's platform viewer count on a modest interval and emits
// the `viewers.changed` bridge event as
// { perAccount: {<accountId>: n}, total, perDestination: [...] }. That one payload object
// also goes to the overlay server's SSE clients as the named `viewers` event, so an overlay
// widget and the dock can never show different numbers.
//
// The per-platform call sits behind StreamProvider::viewerCounts so the poller has no
// per-platform branching: a platform with one channel per account reports a single
// account-wide figure (one API call, as before), while a platform that runs a broadcast
// per stream profile reports one figure per broadcast, so `total` counts every live
// broadcast instead of silently dropping all but one. A platform reporting
// "not live / unsupported" (viewerCounts returns false) is omitted from the aggregate.
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
	void PollAccount(OAuth::OAuthAccount &acct, OAuth::StreamProvider *provider, PollCycle &cycle) override;
	std::optional<json> BuildPayload(PollCycle &&cycle) override;
};

// Process-wide viewer poller accessor (function-local-static singleton, mirroring
// the ChatHub's Chat::Hub() shape so it outlives the detached worker to exit).
ViewerPoller &Viewers();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_VIEWER_POLLER_HPP_
