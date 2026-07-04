#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_KICK_PROVIDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_KICK_PROVIDER_HPP_

#include <memory>
#include <string>

#include "../http_client.hpp"
#include "pkce_loopback.hpp"
#include "provider.hpp"

// The Kick chat/event transports are constructed per account in makeChat/makeEvents,
// defined out-of-line in the .cpp where the complete types are available -- so this
// header only forward-declares the chat transport for the friend grant below (the
// makeChat/makeEvents return types are the base interfaces provider.hpp forward-declares).
namespace Chat {
class KickChat;
}

// The Kick stream provider: auth-code + PKCE over a loopback redirect (Kick is a
// confidential client, so the token calls also carry a client_secret) plus the
// public-API stream-metadata hooks the Go Live modal reads and writes. Kick has
// no advanced metadata fields, so its descriptor exercises the modal's
// empty-advanced path. Endpoints verified against docs.kick.com (2026-06).
namespace OAuth {

// Bumped whenever the requested scope set changes, forcing installs holding
// tokens issued under an older scope set to re-auth (see OAuthAccount::scopeVer).
constexpr int KICK_SCOPE_VERSION = 2;

class KickProvider : public StreamProvider {
public:
	KickProvider();

	std::string id() const override { return "kick"; }
	std::string displayName() const override { return "Kick"; }
	std::string brandColor() const override { return "#53fc18"; }
	int scopeVer() const override { return KICK_SCOPE_VERSION; }

	json capabilityJson() const override;

	AuthStrategy *auth() override { return &auth_; }

	bool fetchIdentity(OAuthAccount &acct, std::string &err) override;
	bool getMetadata(OAuthAccount &acct, json &out, std::string &err) override;
	bool searchCategories(OAuthAccount &acct, const std::string &query, json &out, std::string &err) override;
	bool applyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields,
			   std::string &err) override;
	bool viewerCount(OAuthAccount &acct, int &out, std::string &err) override;

	// The Kick chat transport (Phase 9.0): Pusher read + REST send. A fresh instance
	// per account, run by the ChatHub between go-live and stop. The default
	// chatChannelRef (acct.login = the Kick slug) is what the transport resolves the
	// chatroom id from internally.
	std::unique_ptr<Chat::ChatTransport> makeChat(const OAuthAccount &acct) override;

	// The Kick Pusher event transport (Phase 9.2d): best-effort reverse-engineered
	// sub/gift/host(raid)/(rarely)follow. A fresh instance per account, run by the
	// EventHub on the account-connect lifecycle. events:subscribe is already in kKickScopes.
	std::unique_ptr<Events::EventTransport> makeEvents(const OAuthAccount &acct) override;

private:
	// Send an authenticated Kick request: ensureFresh proactively, stamp the bearer
	// header, and on a 401 force one refresh + retry with the new token. `req` is
	// taken by value so headers are re-applied cleanly on the retry. false + `err`
	// only on a transport failure or an unrecoverable 401; an HTTP error otherwise
	// returns true with the status/body left for the caller to interpret.
	bool SendAuthed(OAuthAccount &acct, Http::HttpReq req, Http::HttpResponse &resp, std::string &err);

	// KickChat sends chat via SendAuthed (same token-coherence path as metadata).
	friend class Chat::KickChat;

	PkceLoopbackStrategy auth_;
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_KICK_PROVIDER_HPP_
