#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_COMMAND_REGISTRY_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_COMMAND_REGISTRY_HPP_

#include "voice/CommandMatcher.hpp"
#include "voice/VoiceListener.hpp"

#include <cstddef>
#include <functional>
#include <string>

namespace Voice {

// whisper's initial_prompt is a bias, not a grammar, and a long one costs tokens from
// the text context. Enough for the command phrases and a busy studio's names.
inline constexpr size_t kMaxPromptChars = 900;

// The scenes, sources and audio channels a command can name right now, and the people and
// platforms a chat message can go to. UI thread.
CommandCandidates CurrentCandidates();

// The whisper prompt: command phrases plus the studio's own names, so "BRB" and
// "Gameplay Cam" come back spelled the way the user named them. Least important first
// and most important last (command phrases, the kPromptPeople most recent chatters
// oldest first, audio channels, the current scene's items, scenes), and bounded by
// kMaxPromptChars by dropping from the FRONT, because whisper keeps the tail of an
// over-long prompt too (Recognizer::SetPrompt). UI thread.
std::string PromptBias();
// The same, for a given studio. Pure.
std::string PromptBiasFor(const CommandCandidates &candidates);

// How many of the most recent chatters' names go into the whisper prompt. Enough for the
// people a streamer is likely to answer; more would push the command phrases out of a
// busy chat's prompt.
inline constexpr size_t kPromptPeople = 20;

// How a dictated chat message goes out: VoiceSettings::sendMode ("countdown", "say" or
// "instant") and countdownSec.
struct SendPolicy {
	std::string mode = "countdown";
	double countdownSec = 3.0;
};

// What a transcript means against a given studio. Pure: the whole decision (muted mic,
// a pending command's yes/no, the matcher, a chat draft and its send mode, the push-to-talk
// fallback to chat, the action envelope) with no libobs.
Interpretation Interpret(const std::string &text, const InterpretContext &ctx, const CommandCandidates &candidates,
			 const SendPolicy &send = SendPolicy());

// The engine's interpreter: Interpret against the studio as it is now. UI thread.
Interpretation InterpretTranscript(const std::string &text, const InterpretContext &ctx);

// The engine's action runner: performs `action` and calls `done` before returning.
// UI thread.
void RunCommand(const PendingAction &action, const std::function<void(bool ok, std::string message)> &done);

// Installs the interpreter, the runner and the prompt source into the engine. Called
// once, from ObsBootstrap::Start, before Voice::Engine().Start().
void InstallCommands();

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_COMMAND_REGISTRY_HPP_
