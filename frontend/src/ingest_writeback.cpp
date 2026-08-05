#include "ingest_writeback.hpp"
#include "event_names.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <memory>

#include <nlohmann/json.hpp>

#include "util/async_task.hpp"
#include "bridge.hpp"
#include "multistream/StreamProfileStore.hpp"
#include "obs_bootstrap.hpp"

namespace {

// Run `fn` against the named profile on the UI thread and block for its answer. Both the
// write and the read below need exactly this, and the shared-promise handshake is the part
// that is easy to get subtly wrong, so it exists once.
//
// A shared promise (NOT captured by reference): if teardown drops the posted task before it
// runs, the task simply never executes and the future times out below -- the promise
// outlives both sides via the shared_ptr, so no dangling set_value on a destroyed object.
template<typename T> T OnProfile(const std::string &profileUuid, T missing, std::function<T(StreamProfile &)> fn)
{
	if (profileUuid.empty()) {
		return missing;
	}
	auto done = std::make_shared<std::promise<T>>();
	std::future<T> fut = done->get_future();

	AsyncTask::PostToUi([done, profileUuid, missing, fn] {
		StreamProfile *p = ObsBootstrap::StreamProfiles().Find(profileUuid);
		done->set_value(p ? fn(*p) : missing);
	});

	if (fut.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
		return missing; // teardown dropped the task
	}
	return fut.get();
}

} // namespace

bool WriteIngestToProfile(const std::string &profileUuid, const std::string &server, const std::string &key)
{
	return OnProfile<bool>(profileUuid, false, [server, key](StreamProfile &p) {
		if (!p.settings) {
			p.settings = obs_data_create();
		}
		if (!server.empty()) {
			obs_data_set_string(p.settings, "server", server.c_str());
		}
		obs_data_set_string(p.settings, "key", key.c_str());
		ObsBootstrap::StreamProfiles().Save();
		Bridge::EmitEvent(EventNames::kStreamProfileChanged, nlohmann::json::object());
		return true;
	});
}

std::string ReadProfileIngestProtocol(const std::string &profileUuid)
{
	return OnProfile<std::string>(profileUuid, std::string(), [](StreamProfile &p) {
		return p.settings ? std::string(obs_data_get_string(p.settings, "protocol")) : std::string();
	});
}
