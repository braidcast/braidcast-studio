#include "OutputBinding.hpp"

#include <util/platform.h>
#include <util/util.hpp>

OutputBinding &OutputBindings::Add(const std::string &canvasUuid)
{
	OutputBinding b;
	BPtr<char> id = os_generate_uuid();
	b.uuid = id.Get();
	b.canvasUuid = canvasUuid;
	bindings.push_back(std::move(b));
	return bindings.back();
}

void OutputBindings::Remove(const std::string &uuid)
{
	for (auto it = bindings.begin(); it != bindings.end(); ++it) {
		if (it->uuid == uuid) {
			bindings.erase(it);
			return;
		}
	}
}

OutputBinding *OutputBindings::Find(const std::string &uuid)
{
	for (OutputBinding &b : bindings) {
		if (b.uuid == uuid) {
			return &b;
		}
	}
	return nullptr;
}

std::vector<OutputBinding *> OutputBindings::ForCanvas(const std::string &canvasUuid)
{
	std::vector<OutputBinding *> out;
	for (OutputBinding &b : bindings) {
		if (b.canvasUuid == canvasUuid) {
			out.push_back(&b);
		}
	}
	return out;
}

bool OutputBindings::AnyEnabledForCanvas(const std::string &canvasUuid) const
{
	for (const OutputBinding &b : bindings) {
		if (b.canvasUuid == canvasUuid && b.enabled) {
			return true;
		}
	}
	return false;
}

bool OutputBindings::AnyEnabled() const
{
	for (const OutputBinding &b : bindings) {
		if (b.enabled) {
			return true;
		}
	}
	return false;
}

bool OutputBindings::ProfileEnabledElsewhere(const std::string &bindingUuid, const std::string &profileUuid) const
{
	if (profileUuid.empty()) {
		return false;
	}
	for (const OutputBinding &b : bindings) {
		if (BindingMatchesProfile(b.uuid, b.profileUuid, b.enabled, bindingUuid, profileUuid)) {
			return true;
		}
	}
	return false;
}

bool OutputBindings::HasPair(const std::string &profileUuid, const std::string &canvasUuid,
			     const std::string &excludeUuid) const
{
	for (const OutputBinding &b : bindings) {
		if (b.uuid != excludeUuid && b.profileUuid == profileUuid && b.canvasUuid == canvasUuid) {
			return true;
		}
	}
	return false;
}

OBSDataArrayAutoRelease OutputBindings::ToDataArray() const
{
	OBSDataArrayAutoRelease arr = obs_data_array_create();
	for (const OutputBinding &b : bindings) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "uuid", b.uuid.c_str());
		obs_data_set_string(item, "profile_uuid", b.profileUuid.c_str());
		obs_data_set_string(item, "canvas_uuid", b.canvasUuid.c_str());
		obs_data_set_bool(item, "enabled", b.enabled);
		obs_data_array_push_back(arr, item);
	}
	return arr;
}

OutputBindings OutputBindings::FromDataArray(obs_data_array_t *arr)
{
	OutputBindings result;
	const size_t count = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease item = obs_data_array_item(arr, i);
		OutputBinding b;
		b.uuid = obs_data_get_string(item, "uuid");
		b.profileUuid = obs_data_get_string(item, "profile_uuid");
		b.canvasUuid = obs_data_get_string(item, "canvas_uuid");
		b.enabled = obs_data_get_bool(item, "enabled");
		if (!b.uuid.empty()) {
			result.bindings.push_back(std::move(b));
		}
	}
	return result;
}
