#include "ingest_writeback.hpp"
#include "event_names.hpp"

#include <chrono>
#include <future>
#include <memory>

#include <nlohmann/json.hpp>

#include "util/async_task.hpp"
#include "bridge.hpp"
#include "multistream/StreamProfileStore.hpp"
#include "obs_bootstrap.hpp"

bool WriteIngestToProfile(const std::string &profileUuid, const std::string &server, const std::string &key)
{
	if (profileUuid.empty()) {
		return false;
	}

	// A shared promise (NOT captured by reference): if teardown drops the posted
	// task before it runs, the task simply never executes and the future times out
	// below -- the promise outlives both sides via the shared_ptr, so no dangling
	// set_value on a destroyed object.
	auto done = std::make_shared<std::promise<bool>>();
	std::future<bool> fut = done->get_future();

	AsyncTask::PostToUi([done, profileUuid, server, key] {
		StreamProfile *p = ObsBootstrap::StreamProfiles().Find(profileUuid);
		if (!p) {
			done->set_value(false);
			return;
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
		done->set_value(true);
	});

	if (fut.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
		return false; // teardown dropped the task
	}
	return fut.get();
}

std::string ReadProfileIngestProtocol(const std::string &profileUuid)
{
	if (profileUuid.empty()) {
		return {};
	}

	// Same shared-promise discipline as the write above: the promise outlives both sides
	// via the shared_ptr, so a task dropped during teardown times out rather than setting
	// a value on a destroyed object.
	auto done = std::make_shared<std::promise<std::string>>();
	std::future<std::string> fut = done->get_future();

	AsyncTask::PostToUi([done, profileUuid] {
		const StreamProfile *p = ObsBootstrap::StreamProfiles().Find(profileUuid);
		if (!p || !p->settings) {
			done->set_value({});
			return;
		}
		const char *protocol = obs_data_get_string(p->settings, "protocol");
		done->set_value(protocol ? std::string(protocol) : std::string());
	});

	if (fut.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
		return {}; // teardown dropped the task
	}
	return fut.get();
}
