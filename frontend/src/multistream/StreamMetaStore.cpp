#include "StreamMetaStore.hpp"

#include "StorePaths.hpp"

#include "../log.hpp"

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
	} catch (const std::exception &e) {
		HostLog(std::string("[storage] stream_meta.json '") + key + "' blob unparseable (" + e.what() +
			"); the remembered defaults it held are gone");
		return json::object();
	}
	return parsed.is_object() ? parsed : json::object();
}

std::string FilePath()
{
	return MultistreamBasicPath("stream_meta.json");
}

} // namespace

void StreamMetaStore::Load()
{
	// Read the two stringified-JSON blobs ("channels" / "streams") from
	// stream_meta.json (key/value envelope like audio_devices.json's "state").
	bool unusable = false;
	OBSDataAutoRelease root = LoadStoreData(FilePath(), &unusable);
	channels_ = ParseObjectBlob(root, "channels");
	streams_ = ParseObjectBlob(root, "streams");
	// A save with nothing changed (a Settings restore, a stream-meta save of the same bags)
	// leaves the file alone. The two passes that change it with no user action gate on
	// LoadedUnusable instead.
	hold_.AfterLoad(unusable, Serialize());
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

void StreamMetaStore::RemoveStreamOverride(const std::string &profileUuid)
{
	streams_.erase(profileUuid);
}

size_t StreamMetaStore::PruneStreamOverrides(const std::function<bool(const std::string &profileUuid)> &isProfile)
{
	size_t removed = 0;
	for (auto it = streams_.begin(); it != streams_.end();) {
		if (isProfile(it.key())) {
			++it;
		} else {
			it = streams_.erase(it);
			removed++;
		}
	}
	return removed;
}

std::string StreamMetaStore::Serialize() const
{
	return channels_.dump() + streams_.dump();
}

bool StreamMetaStore::Save() const
{
	if (hold_.Skips(Serialize())) {
		return true;
	}
	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_string(root, "channels", channels_.dump().c_str());
	obs_data_set_string(root, "streams", streams_.dump().c_str());
	const std::string path = FilePath();
	return ReportSaveResult(SaveJsonAtomic(root, path), path);
}
