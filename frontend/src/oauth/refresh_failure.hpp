#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_REFRESH_FAILURE_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_REFRESH_FAILURE_HPP_

#include <string>

// The refresh-rejection classification rule. Every provider refreshes through the
// same BrokerStrategy, so this is the single place that decides whether a failed
// refresh means the credential is gone or merely that the request didn't land.
// Header-only and dependency-free so the rule can be exercised in isolation.
namespace OAuth {

// Why a refresh attempt failed.
//   Transient -- the failure says nothing about the credential (transport error,
//     timeout, rate limit, broker/provider fault). Retry later; the account keeps
//     its connected state.
//   Dead -- the refresh token itself is revoked/expired/rotated away. It cannot be
//     silently re-granted (re-auth needs user consent in a browser), so the account
//     is flagged needs-reconnect.
enum class RefreshFailureKind { Transient, Dead };

inline const char *RefreshFailureKindName(RefreshFailureKind kind)
{
	return kind == RefreshFailureKind::Dead ? "dead" : "transient";
}

// Classify one refresh rejection. `httpStatus` is the broker's HTTP status, or 0 for
// a transport-level failure (no response at all); `errorCode` is the OAuth2 `error`
// field parsed out of the response body (RFC 6749 sec 5.2), empty when the body
// carried none or was unparseable.
//
// The asymmetry is deliberate: flagging a live account dead strands a user who can
// still stream, while retrying a genuinely dead token costs one wasted request. So
// only the one code RFC 6749 defines as "the grant is no longer valid" returns Dead,
// and everything else -- including an unrecognized code -- stays Transient.
inline RefreshFailureKind ClassifyRefreshFailure(int httpStatus, const std::string &errorCode)
{
	// Status first, before the body is consulted at all: a 5xx/timeout/rate-limit
	// response body is not a credential verdict, so an error page or a proxied
	// payload that happens to echo "invalid_grant" must not strand the account.
	if (httpStatus == 0 || httpStatus >= 500 || httpStatus == 408 || httpStatus == 429) {
		return RefreshFailureKind::Transient;
	}
	// invalid_grant is the only code that means the refresh token is gone. Others
	// (invalid_client, unauthorized_client, invalid_request) indicate a broker/app
	// misconfiguration and leave the user's credential unproven -- treating those as
	// dead would sign every user out on a broker deploy mistake.
	return errorCode == "invalid_grant" ? RefreshFailureKind::Dead : RefreshFailureKind::Transient;
}

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_REFRESH_FAILURE_HPP_
