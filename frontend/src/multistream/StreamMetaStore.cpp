#include "StreamMetaStore.hpp"

#include "StorePaths.hpp"

#include <obs.h>
#include <obs.hpp>

using json = nlohmann::json;

namespace {

// Parse the string blob under `key` in `root` into a JSON object, defaulting to
// an empty object when the key is missing, empty, unparseable, or not an object.
json ParseObjectBlob(obs_data_t *root, const char *key)
{
	const char *v = root ? obs_data_get_string(root, key) : nullptr;
	if (!v || !*v) {
		return json::object();
	}
	json parsed;
	try {
		parsed = json::parse(v);
	} catch (...) {
		return json::object();
	}
	return parsed.is_object() ? parsed : json::object();
}

} // namespace

void StreamMetaStore::Load()
{
	// Read the two stringified-JSON blobs ("channels" / "streams") from
	// stream_meta.json (key/value envelope like audio_devices.json's "state").
	OBSDataAutoRelease root =
		obs_data_create_from_json_file_safe(MultistreamBasicPath("stream_meta.json").c_str(), "bak");
	channels_ = ParseObjectBlob(root, "channels");
	streams_ = ParseObjectBlob(root, "streams");
}

json StreamMetaStore::ChannelDefaults(const std::string &accountId) const
{
	const auto it = channels_.find(accountId);
	return it != channels_.end() ? *it : json::object();
}

json StreamMetaStore::StreamOverride(const std::string &profileUuid) const
{
	const auto it = streams_.find(profileUuid);
	return it != streams_.end() ? *it : json::object();
}

void StreamMetaStore::PutChannelDefaults(const std::string &accountId, const json &fields)
{
	channels_[accountId] = fields;
}

void StreamMetaStore::PutStreamOverride(const std::string &profileUuid, const json &fields)
{
	streams_[profileUuid] = fields;
}

void StreamMetaStore::Save() const
{
	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_string(root, "channels", channels_.dump().c_str());
	obs_data_set_string(root, "streams", streams_.dump().c_str());
	SaveJsonAtomic(root, MultistreamBasicPath("stream_meta.json"));
}
