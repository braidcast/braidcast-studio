#pragma once

#include <string>

// Automatic, persistent fallback to software (SwiftShader) rendering when CEF's
// GPU subprocess crash-loops. On some machines (notably a GPU newer than this
// libcef) the GPU process CHECK()-fails on launch with EXCEPTION_BREAKPOINT,
// Chromium restarts it, it crashes again, and the renderer never composites -- so
// the UI paints as a blank window. Launching with --disable-gpu avoids the crash
// entirely (the app ships vk_swiftshader.dll for software compositing).
//
// The decision is driven by a per-boot sentinel file: it is written before the
// GPU-backed browser is created and deleted once the renderer confirms a paint. If
// it survives to the next boot, the previous run started the browser but never
// composited (the crash signature), so that boot falls back to software rendering
// and persists the choice.
namespace GpuSafeMode {

struct BootDecision {
	// Run this boot with the GPU disabled (software rendering).
	bool disableGpu = false;
	// This boot fell back automatically after detecting a prior GPU crash (drives a
	// user-visible notice); false for manual/persisted software mode.
	bool autoFellBack = false;
	// Absolute path of the persisted safe-mode flag, for the fallback notice.
	std::string safeModeFile;
};

// Resolve the rendering mode for this launch. MUST be called on the main thread
// before CefInitialize (it reads/writes the sentinel and may arm the probe). See
// the state machine in gpu_safe_mode.cpp.
BootDecision DecideAtBoot();

// Clear the boot sentinel: proof the renderer composited this run, so the GPU path
// is healthy. Call from the main browser's load-completion hook. UI-thread only
// and idempotent (no-op after the first call and when no sentinel exists).
void NotePaintSuccess();

} // namespace GpuSafeMode
