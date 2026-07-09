#include "provider.hpp"

#include "../chat/chat_transport.hpp"
#include "../events/event_transport.hpp"

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

} // namespace OAuth
