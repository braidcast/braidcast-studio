#include "ingest_writeback.hpp"
#include "event_names.hpp"

#include <chrono>
#include <optional>

#include <nlohmann/json.hpp>

#include "util/async_task.hpp"
#include "bridge.hpp"
#include "multistream/StreamProfileStore.hpp"
#include "obs_bootstrap.hpp"

namespace {

// Bound on one UI-thread hop. A task dropped at teardown never resolves, so this
// is also the full stall an in-flight call pays on the way down.
constexpr std::chrono::seconds kUiCallTimeout{10};

} // namespace

bool WriteIngestToProfile(const std::string &profileUuid, const std::string &server, const std::string &key)
{
	if (profileUuid.empty()) {
		return false;
	}

	const std::optional<bool> written = AsyncTask::CallOnUiWithTimeout<bool>(
		[profileUuid, server, key] {
			StreamProfile *p = ObsBootstrap::StreamProfiles().Find(profileUuid);
			if (!p) {
				return false;
			}
			if (!p->settings) {
				p->settings = obs_data_create();
			}
			if (!server.empty()) {
				obs_data_set_string(p->settings, "server", server.c_str());
			}
			obs_data_set_string(p->settings, "key", key.c_str());
			ObsBootstrap::StreamProfiles().Save();
			Bridge::EmitEvent(EventNames::kStreamProfileChanged, nlohmann::json::object());
			return true;
		},
		kUiCallTimeout);

	// nullopt: no answer within the budget. The write may still land afterwards,
	// so this reports "not confirmed", not "not written".
	return written.value_or(false);
}

std::string ReadProfileIngestProtocol(const std::string &profileUuid)
{
	if (profileUuid.empty()) {
		return {};
	}

	const std::optional<std::string> protocol = AsyncTask::CallOnUiWithTimeout<std::string>(
		[profileUuid] {
			const StreamProfile *p = ObsBootstrap::StreamProfiles().Find(profileUuid);
			if (!p || !p->settings) {
				return std::string();
			}
			const char *value = obs_data_get_string(p->settings, "protocol");
			return value ? std::string(value) : std::string();
		},
		kUiCallTimeout);

	return protocol.value_or(std::string()); // nullopt: no answer in time, same as "unset"
}
