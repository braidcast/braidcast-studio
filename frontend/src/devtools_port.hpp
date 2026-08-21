#pragma once

#include <cstdint>
#include <string>

// CEF's built-in remote-debugging (Chrome DevTools Protocol) listener, gated
// behind an opt-in named in the environment.
//
// The opt-in is BRAIDCAST_DEBUG truthy AND BRAIDCAST_DEBUG_COMPONENTS containing
// "devtools" -- a non-category token on the same two-var scheme as "gpudiag". The
// master alone must not open it, for the same reason the render gates are held out
// of "basic": a debug flag people leave on should not silently widen the attack
// surface. A CDP port is arbitrary JavaScript execution inside the webview's
// origin, and that origin holds OAuth tokens and resolved stream keys.
//
// Environment only, and boot only. It deliberately does not consult the persisted
// DiagnosticsSettings toggle -- opening a debug port should take a decision on this
// machine in a file the developer edits, not a UI switch that can be flipped and
// forgotten. It cannot be opened or closed later either: CefSettings is read once,
// at CefInitialize.
//
// The gate is also the last word against the process's own command line: on Windows
// CefMainArgs carries only an HINSTANCE, so Chromium builds its CommandLine from
// GetCommandLineW() and would otherwise honor a --remote-debugging-port switch this
// app never authorized. App::OnBeforeCommandLineProcessing strips those switches and
// re-appends whatever Active() decided.
namespace DevToolsPort {

// The TCP port to hand CefSettings.remote_debugging_port, or 0 when the opt-in is
// absent. Reads only the environment, so it answers the same before and after
// DecideAtBoot(). Warns and falls back to the default when
// BRAIDCAST_DEVTOOLS_PORT names something CEF would reject.
uint16_t Resolve();

// Resolve() once and stash the answer for Active(). MUST be called on the main
// thread before CefSettings is populated.
uint16_t DecideAtBoot();

// The port this launch authorized, or 0 when none. The seam the mandatory UI
// indicator reads (through diagnostics.get's "devToolsPort"). Valid after
// DecideAtBoot(). Answers "did this install opt into a debug port", which is a
// question about the gate, not about a socket -- nothing observed at runtime is
// allowed to lower it.
uint16_t Active();

// Raise a debug-port warning that has to survive the boot ordering. Emitted at once
// (for whoever is watching stderr) AND queued for FlushPendingWarnings(): every
// decision about this port is taken before ObsBootstrap::Start() installs the
// session-log handler, so an immediate blog() alone reaches a console and never a
// file -- and a finding that never lands on disk is not a finding. A message already
// raised this process is dropped, so a path that runs twice (the self-test resolves the
// port a second time, after the flush) cannot double-log it.
void Warn(const std::string &message);

// Emit every message Warn() has raised that has not been emitted here yet. Call once,
// immediately after SessionLog::Init().
void FlushPendingWarnings();

} // namespace DevToolsPort
