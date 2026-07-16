#include "gpu_safe_mode.hpp"

#include <util/platform.h>

#include <filesystem>
#include <fstream>
#include <system_error>

#include "multistream/StorePaths.hpp"

namespace {

// Per-boot sentinel: written before the GPU-backed browser is created, deleted on
// the first confirmed paint. Its survival to the next boot is the GPU-crash signal.
constexpr char kSentinelFile[] = "gpu-probe.lock";

// Sticky safe-mode flag: once a GPU crash is detected the app persists this so every
// later boot stays on software rendering without re-probing (and re-crashing).
constexpr char kSafeModeFile[] = "gpu-safe-mode.on";

// User-createable override next to the exe -- mirrors the portable marker. Its mere
// presence forces software rendering, the escape hatch for a blank-screen install
// whose UI never comes up.
constexpr wchar_t kDisableGpuMarker[] = L"braidcast_disable_gpu.txt";

// UI-thread-only latch so repeated main-frame loads (e.g. detached windows) clear
// the sentinel just once.
bool g_sentinelCleared = false;

bool FileExists(const std::string &absPath)
{
	if (absPath.empty()) {
		return false;
	}
	std::error_code ec;
	return std::filesystem::exists(std::filesystem::u8path(absPath), ec);
}

void WriteEmptyFile(const std::string &absPath)
{
	if (absPath.empty()) {
		return;
	}
	std::ofstream(std::filesystem::u8path(absPath), std::ios::binary | std::ios::trunc);
}

void RemoveFile(const std::string &absPath)
{
	if (absPath.empty()) {
		return;
	}
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(absPath), ec);
}

} // namespace

GpuSafeMode::BootDecision GpuSafeMode::DecideAtBoot()
{
	BootDecision decision;

	// Manual override beside the exe wins unconditionally and needs no config dir.
	if (MarkerFileBesideExe(kDisableGpuMarker)) {
		decision.disableGpu = true;
		return decision;
	}

	const std::string &base = BraidcastConfigDir();
	if (base.empty()) {
		// No writable config base -> can't run the sentinel probe; leave GPU enabled.
		return decision;
	}
	os_mkdirs(base.c_str());

	const std::string sentinel = BraidcastConfigPath(kSentinelFile);
	const std::string safeMode = BraidcastConfigPath(kSafeModeFile);
	decision.safeModeFile = safeMode;

	// A previously-persisted crash forces software rendering with no probe.
	if (FileExists(safeMode)) {
		decision.disableGpu = true;
		return decision;
	}

	// A sentinel left from the previous run means that run started the GPU-backed
	// browser but never confirmed a paint: almost certainly the GPU crash loop. Fall
	// back now, persist it so future boots skip the probe, and clear the stale file.
	if (FileExists(sentinel)) {
		decision.disableGpu = true;
		decision.autoFellBack = true;
		WriteEmptyFile(safeMode);
		RemoveFile(sentinel);
		return decision;
	}

	// Healthy path: arm the probe for this GPU-enabled boot. NotePaintSuccess() clears
	// it once the renderer composites, proving the GPU path works this run.
	WriteEmptyFile(sentinel);
	return decision;
}

void GpuSafeMode::NotePaintSuccess()
{
	if (g_sentinelCleared) {
		return;
	}
	g_sentinelCleared = true;
	RemoveFile(BraidcastConfigPath(kSentinelFile));
}
