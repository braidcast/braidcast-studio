#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_LOOPBACK_LISTENER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_LOOPBACK_LISTENER_HPP_

#include <functional>
#include <map>
#include <memory>
#include <string>

// A one-shot 127.0.0.1(+[::1]) loopback HTTP listener for an OAuth redirect. Binds an
// ephemeral port, waits for the ONE request carrying code=/error=, exposes its query
// params, and writes a success/failure page back to the browser. Winsock details stay
// inside the .cpp. Reused by BrokerStrategy (the loopback is now the broker->app final hop).
namespace OAuth {

class LoopbackListener {
public:
	LoopbackListener();
	~LoopbackListener();
	LoopbackListener(const LoopbackListener &) = delete;
	LoopbackListener &operator=(const LoopbackListener &) = delete;

	// Bind 127.0.0.1:0 (+ best-effort [::1] on the same port). false + err on failure.
	bool Bind(std::string &err);

	// The OS-assigned port (valid after Bind).
	unsigned Port() const;

	// Block until a redirect carrying code=/error= arrives, honoring `canceled()` and a
	// `timeoutSec` deadline (re-checked every 0.5s). Fills the query params. Returns false
	// (err set, or empty on cancel) on timeout/cancel/socket error.
	bool Await(const std::function<bool()> &canceled, int timeoutSec, std::string &err);

	// The captured query params (valid after a true Await).
	const std::map<std::string, std::string> &Params() const;

	// Write the terminal HTML page to the held browser connection and close it.
	// Only valid after Await() returned true (else the connection socket is invalid).
	void Respond(bool ok);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_LOOPBACK_LISTENER_HPP_
