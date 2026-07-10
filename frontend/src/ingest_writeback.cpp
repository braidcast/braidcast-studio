#include "ingest_writeback.hpp"
#include "event_names.hpp"

#include <chrono>
#include <future>
#include <memory>

#include <nlohmann/json.hpp>

#include "async_task.hpp"
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
