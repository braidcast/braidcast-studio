#pragma once

#include <string>

// Build-time-injected platform OAuth credentials. The client_id is public (it
// ships in the binary either as plaintext or lightly obfuscated to deter casual
// scraping); the device-code flow uses no secret. Returns "" when no credential
// was supplied at configure time.
std::string TwitchClientId();
bool TwitchConfigured();

// Kick is a confidential client: both id and secret ship in the binary
// (plaintext or lightly obfuscated). Configured = both non-empty.
std::string KickClientId();
std::string KickClientSecret();
bool KickConfigured();

// YouTube (Google) desktop client: the id is required; the secret ships
// non-confidential and is OPTIONAL (the PKCE token calls send it only when set).
// Configured = id non-empty (the secret may legitimately be empty).
std::string YouTubeClientId();
std::string YouTubeClientSecret();
bool YouTubeConfigured();
