#ifndef OBS_MULTISTREAM_FRONTEND_ENV_CONFIG_HPP_
#define OBS_MULTISTREAM_FRONTEND_ENV_CONFIG_HPP_

#include <optional>
#include <string>

// The one place frontend/src/ reads the process environment. Every flag used to
// have its own getenv() + truthy-parser pair, and only one of them also consulted
// the repo-root .env -- so .env worked for that one flag and silently did nothing
// for the rest. Centralized here so every flag gets the same precedence and the
// same truthy grammar.
namespace Env {

// Raw string value: the process environment wins; else the same key in the
// gitignored repo-root .env (dev builds only -- BRAIDCAST_ENV_FILE is baked at
// configure time and absent in CI/shipped builds); else nullopt.
std::optional<std::string> Raw(const char *key);

// Truthy grammar: 0/false/no/off (case-insensitive) and the empty value are a
// denial; any other value that is set at all enables. That keeps every token the
// parsers this replaces treated as enabling -- including the diag flags' "set to
// anything" convention -- while making an explicit `KEY=false` mean off, which one
// of those parsers got wrong. Absent -> fallback.
bool Flag(const char *key, bool fallback = false);

// Base-10 integer value; fallback when absent or unparseable. A positivity/floor
// requirement (e.g. a poll interval) is caller-specific and applied by the caller.
long Number(const char *key, long fallback);

// Floating-point value; fallback when absent or unparseable.
double Double(const char *key, double fallback);

// True when `key` is present, even set to an empty value -- matches the historical
// `getenv(key) != nullptr` presence checks this replaces. Routing FE_SMOKE_QUIT_SECONDS
// through this (and through Number/Raw) means a stray line in .env now arms the
// self-terminating smoke path -- which also writes the real %APPDATA% config -- on
// every dev launch, so keep that file clean.
bool IsSet(const char *key);

// String value, or fallback when absent.
std::string Value(const char *key, const std::string &fallback = {});

} // namespace Env

#endif // OBS_MULTISTREAM_FRONTEND_ENV_CONFIG_HPP_
