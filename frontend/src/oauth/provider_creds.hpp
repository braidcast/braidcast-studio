#pragma once

#include <string>

// Build-time-injected platform OAuth credentials. The client_id is public (it
// ships in the binary either as plaintext or lightly obfuscated to deter casual
// scraping) and is sent as Helix's Client-Id header; the sign-in itself runs
// through the broker, which holds the secret. Returns "" when no credential was
// supplied at configure time.
std::string TwitchClientId();
bool TwitchConfigured();
