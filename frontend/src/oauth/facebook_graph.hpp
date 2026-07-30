#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_FACEBOOK_GRAPH_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_FACEBOOK_GRAPH_HPP_

#include <string>

#include "provider.hpp"

// The Graph API access seam every Facebook subsystem rides: the pinned API version, the
// URL builder, and the Page-scoped scratch account a Page call presents. One definition,
// because a second copy of the version pin is a silent version split between two
// subsystems talking to the same API.
namespace OAuth {

// Pinned deliberately. Meta keeps a Graph version working for about two years after its
// successor ships and then removes it, so an unpinned "latest" would silently change
// request and response shapes under an already-shipped build. Moving forward is an edit
// here plus a re-test, never something that happens on its own.
constexpr const char *kGraphVersion = "v25.0";

inline std::string GraphUrl(const std::string &path)
{
	return std::string("https://graph.facebook.com/") + kGraphVersion + "/" + path;
}

// A scratch account carrying ONE Page's access token, so a Page-scoped call rides the
// shared SendAuthed / SendAuthedStreaming transport instead of hand-rolling its own auth
// header. A Page token is a different credential from the user token the stored record
// holds, and a copy rather than a mutation keeps that record intact. Nothing here can be
// written back: this strategy issues no refresh token, so ensureFresh returns before it
// would touch the account store.
inline OAuthAccount PageAccount(const std::string &providerId, const std::string &pageToken)
{
	OAuthAccount page;
	page.providerId = providerId;
	page.access = pageToken;
	return page;
}

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_FACEBOOK_GRAPH_HPP_
