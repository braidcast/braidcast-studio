#include "provider_creds.hpp"

#include "obf.h"
#include "ui-config.h"

// TWITCH_HASH == 0 means the client_id was injected as plaintext (e.g. a dev
// passing -DTWITCH_CLIENTID= via .env): return it verbatim. A non-zero hash is
// the nibble-XOR key used to obfuscate the stored bytes, so deobfuscate in place
// before returning. deobfuscate_str mutates the buffer, so operate on a copy.
std::string TwitchClientId()
{
	std::string id = TWITCH_CLIENTID;
	if (id.empty())
		return id;
	if (TWITCH_HASH == 0)
		return id;
	deobfuscate_str(&id[0], TWITCH_HASH);
	return id;
}

bool TwitchConfigured()
{
	return !TwitchClientId().empty();
}

// KICK_HASH / KICK_SECRET_HASH == 0 means the value was injected as plaintext;
// a non-zero hash is the nibble-XOR key. deobfuscate_str mutates in place, so
// operate on a copy. Mirrors TwitchClientId.
std::string KickClientId()
{
	std::string id = KICK_CLIENTID;
	if (id.empty())
		return id;
	if (KICK_HASH == 0)
		return id;
	deobfuscate_str(&id[0], KICK_HASH);
	return id;
}

std::string KickClientSecret()
{
	std::string secret = KICK_SECRET;
	if (secret.empty())
		return secret;
	if (KICK_SECRET_HASH == 0)
		return secret;
	deobfuscate_str(&secret[0], KICK_SECRET_HASH);
	return secret;
}

bool KickConfigured()
{
	return !KickClientId().empty() && !KickClientSecret().empty();
}
