#ifndef OBS_MULTISTREAM_FRONTEND_OP_ERROR_HPP_
#define OBS_MULTISTREAM_FRONTEND_OP_ERROR_HPP_

#include <string>

// A bridge failure travels through one string slot (each method's `error`
// out-param, then the CEF Failure payload), but serves two audiences: logs want
// the full diagnostic chain ("streamMeta.set: YouTube liveBroadcasts.insert
// failed: ..."), the UI wants only the human sentence at its end. These helpers
// carry both in that one slot: User() packs {diagnostic, user message} into a
// JSON envelope keyed by an explicit discriminator, Wrap() prefixes context onto
// the DIAGNOSTIC only (intermediate layers keep adding log context without
// destroying the user message), Diagnostic()/UserMessage() unpack at the sinks.
// A plain string passes through every helper unchanged, so the many bridge
// methods that never call User() behave exactly as before.
namespace Err {

// Pack a diagnostic and its user-facing sentence into one transport string.
// Called at the producer that actually knows a streamer-readable reason (e.g.
// the YouTube quota preflight); everything upstream wraps or forwards it.
std::string User(const std::string &diagnostic, const std::string &userMessage);

// Prepend step/method context to the DIAGNOSTIC only, preserving any user
// message. A plain (non-envelope) `err` yields a plain prefixed string -- this
// never manufactures an envelope.
std::string Wrap(const std::string &prefix, const std::string &err);

// The full diagnostic chain, for logs and protocol surfaces. Plain string in ->
// the same string out.
std::string Diagnostic(const std::string &err);

// The user-facing message, or "" when the error never carried one.
std::string UserMessage(const std::string &err);

} // namespace Err

#endif // OBS_MULTISTREAM_FRONTEND_OP_ERROR_HPP_
