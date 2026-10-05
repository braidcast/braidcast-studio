#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

// The in-app update check (roadmap 10.6) for direct-download installs: once per launch, a
// short while after boot, ask GitHub for the latest PUBLISHED release, and when it is newer
// than this build, hold a notice for the page to show -- once per version, never again
// after it has been shown. Best-effort throughout: it never blocks boot or streaming, and a
// failure is a log line. No download, no install: the notice links to the download page.
//
// It does not run at all in a Store (MSIX) install, which the Store keeps up to date, in a
// headless smoke run, or with GeneralSettings::checkForUpdates off.
namespace Update {

class UpdateChecker {
public:
	// Boot: start the one background check, unless it is turned off or does not apply.
	void Start(bool enabled);
	// Shutdown: a check still waiting or in flight drops its result.
	void Stop();

	// The notice waiting to be shown, {version, url}, or null. The page asks at load and
	// on update.available, so a check that finished before the page did is not lost.
	nlohmann::json Pending() const;
	// The page showed the notice for `version`: never show it again (persisted).
	void Acknowledge(const std::string &version);

private:
	void Run(const std::shared_ptr<std::atomic<bool>> &stop);
	std::string Notified() const; // the last version shown, from update_check.json

	mutable std::mutex mutex_;
	std::shared_ptr<std::atomic<bool>> stop_; // guarded by mutex_
	std::string pendingVersion_;              // guarded by mutex_; "" when none
};

UpdateChecker &Checker();

} // namespace Update
