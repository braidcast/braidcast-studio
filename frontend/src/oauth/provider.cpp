#include "provider.hpp"

#include "../chat/chat_transport.hpp"
#include "../events/event_transport.hpp"
#include "util/http_client.hpp"

// The base StreamProvider's transport factories default to "no transport". They are
// defined here rather than inline in the header because the return type is a
// std::unique_ptr<> of a type the header only forward-declares (to break the
// chat_transport.hpp <-> provider.hpp include cycle); unique_ptr's default_delete
// needs the complete type, which this translation unit sees via the includes above.
namespace OAuth {

std::unique_ptr<Chat::ChatTransport> StreamProvider::makeChat(const OAuthAccount &acct)
{
	(void)acct;
	return nullptr;
}

std::unique_ptr<Events::EventTransport> StreamProvider::makeEvents(const OAuthAccount &acct)
{
	(void)acct;
	return nullptr;
}

bool StreamProvider::ensureIdentity(OAuthAccount &acct, std::string &err)
{
	if (!acct.userId.empty()) {
		return true;
	}
	return fetchIdentity(acct, err);
}

void StreamProvider::stampAuth(Http::HttpReq &r, const OAuthAccount &acct) const
{
	r.headers.push_back("Authorization: Bearer " + acct.access);
}

bool StreamProvider::SendAuthed(OAuthAccount &acct, Http::HttpReq req, Http::HttpResponse &resp, std::string &err)
{
	// Proactive refresh inside the skew window (best-effort: if it fails the token
	// may still be valid, so we let the request proceed and rely on the 401 path).
	std::string freshErr;
	auth()->ensureFresh(acct, freshErr);

	Http::HttpReq attempt = req;
	stampAuth(attempt, acct);
	resp = Http::HttpRequest(attempt);
	if (resp.status == 0) {
		err = displayName() + " request failed: " + resp.error;
		return false;
	}
	if (resp.status != 401) {
		return true;
	}

	// Reactive 401: force one refresh + retry with the new bearer. Route through
	// ensureFresh(force) -- NOT a bare refresh() -- so a rotated refresh token is
	// re-read + written back under the same single-flight lock the proactive path uses.
	// Kick rotates its refresh token on every refresh, so a bare refresh() would rotate
	// it in memory and drop the new token, bricking the account on the next refresh;
	// ensureFresh(force) keeps every provider on one store-coherent path (benign for
	// Twitch, whose refresh tokens do not rotate).
	std::string refreshErr;
	if (!auth()->ensureFresh(acct, refreshErr, /*force=*/true)) {
		err = "re-authentication required";
		return false;
	}
	Http::HttpReq retry = req;
	stampAuth(retry, acct);
	resp = Http::HttpRequest(retry);
	if (resp.status == 0) {
		err = displayName() + " request failed: " + resp.error;
		return false;
	}
	if (resp.status == 401) {
		err = "re-authentication required";
		return false;
	}
	return true;
}

long StreamProvider::SendAuthedStreaming(OAuthAccount &acct, Http::HttpReq req,
					 const std::function<bool(std::string_view chunk)> &onChunk,
					 std::string &errorBody, std::string &err)
{
	// Proactive refresh inside the skew window (best-effort, mirroring SendAuthed): if it
	// fails the token may still be valid, so proceed and rely on the 401 path below.
	std::string freshErr;
	auth()->ensureFresh(acct, freshErr);

	Http::HttpReq attempt = req;
	stampAuth(attempt, acct);
	errorBody.clear();
	long status = Http::HttpRequestStreaming(attempt, onChunk, errorBody, err);
	if (status == 0) {
		err = displayName() + " request failed: " + err;
		return 0;
	}
	if (status != 401) {
		return status;
	}

	// Reactive 401: force one refresh + retry with the new bearer. A non-2xx body is not
	// streamed to onChunk (HttpRequestStreaming captured it into errorBody instead), so the
	// caller has emitted nothing yet and the retry starts clean. Route through
	// ensureFresh(force) for the same store-coherent single-flight path SendAuthed uses.
	std::string refreshErr;
	if (!auth()->ensureFresh(acct, refreshErr, /*force=*/true)) {
		err = "re-authentication required";
		return 401;
	}
	Http::HttpReq retry = req;
	stampAuth(retry, acct);
	errorBody.clear();
	status = Http::HttpRequestStreaming(retry, onChunk, errorBody, err);
	if (status == 0) {
		err = displayName() + " request failed: " + err;
		return 0;
	}
	if (status == 401) {
		err = "re-authentication required";
	}
	return status;
}

} // namespace OAuth
