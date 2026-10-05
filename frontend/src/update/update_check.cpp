#include "update_check.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>

#include <windows.h>

#include "update_version.hpp"
#include "../bridge.hpp"
#include "../chat/ws_client.hpp" // Chat::CancelableSleep
#include "../event_names.hpp"
#include "../log.hpp"
#include "../multistream/StorePaths.hpp"
#include "../util/async_task.hpp"
#include "../util/file_util.hpp"
#include "../util/http_client.hpp"

#include <obs.h>

using json = nlohmann::json;

namespace Update {

namespace {

// The latest published release. GitHub's /releases/latest already leaves out drafts and
// pre-releases, which is the gate the design asks for: while every release is a draft the
// check finds nothing, and it starts working with the first publish.
constexpr const char *kLatestReleaseUrl = "https://api.github.com/repos/braidcast/braidcast-studio/releases/latest";
// Where the notice sends the user: the release-gated download page, not the asset.
constexpr const char *kDownloadUrl = "https://braidcast.com/download";
constexpr const char *kStateFile = "update_check.json";
// Long enough that boot, the canvases and the first go-live checks have the network to
// themselves; the notice is not urgent.
constexpr std::chrono::seconds kDelayAfterBoot{20};
constexpr int kFetchTimeoutSec = 15;

// A Store (MSIX) install has a package identity, and the Store updates it. Looked up at run
// time rather than through appmodel.h, which needs a Windows 8 target to declare it.
bool IsPackaged()
{
	using GetPackageNameFn = LONG(WINAPI *)(UINT32 *, PWSTR);
	constexpr LONG kNoPackage = 15700L; // APPMODEL_ERROR_NO_PACKAGE
	const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	const auto get =
		kernel ? reinterpret_cast<GetPackageNameFn>(GetProcAddress(kernel, "GetCurrentPackageFullName"))
		       : nullptr;
	if (!get) {
		return false;
	}
	UINT32 length = 0;
	return get(&length, nullptr) != kNoPackage;
}

// A headless smoke run (FE_SMOKE_QUIT_SECONDS) or a self-test drive must not reach out.
bool IsHeadlessRun()
{
	return std::getenv("FE_SMOKE_QUIT_SECONDS") != nullptr || std::getenv("BRAIDCAST_SELFTEST_STREAM") != nullptr;
}

} // namespace

void UpdateChecker::Start(bool enabled)
{
	if (!enabled) {
		HostLog("[update] check off (Settings > General)");
		return;
	}
	if (IsPackaged()) {
		HostLog("[update] Store install; the Store keeps it up to date");
		return;
	}
	if (IsHeadlessRun()) {
		return;
	}
	Stop();
	auto stop = std::make_shared<std::atomic<bool>>(false);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = stop;
	}
	AsyncTask::RunAsync([this, stop] { Run(stop); });
}

void UpdateChecker::Stop()
{
	std::shared_ptr<std::atomic<bool>> stop;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop = std::move(stop_);
	}
	if (stop) {
		stop->store(true, std::memory_order_release);
	}
}

json UpdateChecker::Pending() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (pendingVersion_.empty()) {
		return nullptr;
	}
	return json{{"version", pendingVersion_}, {"url", kDownloadUrl}};
}

void UpdateChecker::Acknowledge(const std::string &version)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (version.empty() || version != pendingVersion_) {
			return;
		}
		pendingVersion_.clear();
	}
	const std::string body = json{{"notified", version}}.dump(2);
	if (FileUtil::WriteBinaryFileAtomic(BraidcastConfigPath(kStateFile), body.data(), body.size()) !=
	    FileUtil::AtomicWriteResult::Ok) {
		HostLog("[update] could not save " + std::string(kStateFile) + "; the notice may show again");
	}
}

std::string UpdateChecker::Notified() const
{
	const std::optional<std::string> text = FileUtil::ReadUtf8File(BraidcastConfigPath(kStateFile));
	if (!text) {
		return std::string();
	}
	const json j = json::parse(*text, nullptr, false);
	return j.is_object() ? j.value("notified", std::string()) : std::string();
}

void UpdateChecker::Run(const std::shared_ptr<std::atomic<bool>> &stop)
{
	const auto canceled = [stop] {
		return stop->load(std::memory_order_acquire);
	};
	if (Chat::CancelableSleep(kDelayAfterBoot, canceled)) {
		return;
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url = kLatestReleaseUrl;
	req.timeoutSec = kFetchTimeoutSec;
	// GitHub refuses an API request with no User-Agent.
	req.headers.push_back("User-Agent: Braidcast-UpdateCheck");
	req.headers.push_back("Accept: application/vnd.github+json");
	const Http::HttpResponse resp = Http::HttpRequest(req);
	if (canceled()) {
		return;
	}
	if (!resp.error.empty()) {
		HostLog("[update] check failed: " + resp.error);
		return;
	}
	if (resp.status == 404) {
		// No published release yet (every release a draft), or the repository is not public.
		HostLog("[update] no published release to compare against");
		return;
	}
	if (resp.status < 200 || resp.status >= 300) {
		HostLog("[update] check failed: HTTP " + std::to_string(resp.status));
		return;
	}
	const json release = json::parse(resp.body, nullptr, false);
	if (!release.is_object() || release.value("draft", false) || release.value("prerelease", false)) {
		HostLog("[update] the latest release did not read as a published one");
		return;
	}
	const std::string tag = release.value("tag_name", std::string());
	const char *running = obs_get_version_string();
	const std::string current = running ? running : "";
	if (!IsNewer(tag, current)) {
		HostLog("[update] up to date (" + current + "; latest " + (tag.empty() ? "unknown" : tag) + ")");
		return;
	}
	if (tag == Notified()) {
		HostLog("[update] " + tag + " is available; already shown");
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		pendingVersion_ = tag;
	}
	HostLog("[update] " + tag + " is available (running " + current + ")");
	Bridge::EmitEvent(EventNames::kUpdateAvailable, Pending());
}

UpdateChecker &Checker()
{
	static UpdateChecker checker;
	return checker;
}

} // namespace Update
