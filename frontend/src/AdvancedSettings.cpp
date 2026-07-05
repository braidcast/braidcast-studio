#include "AdvancedSettings.hpp"

#include "multistream/StorePaths.hpp"

#include <obs.hpp>
#include <util/platform.h>

#ifdef _WIN32
#include <windows.h>
#endif

void AdvancedSettings::Load()
{
	OBSDataAutoRelease root =
		obs_data_create_from_json_file_safe(MultistreamBasicPath("advanced.json").c_str(), "bak");
	if (!root) {
		return; // no file yet: keep struct defaults
	}
	for (const AdvancedBoolField &f : kAdvancedBoolFields) {
		obs_data_set_default_bool(root, f.file, this->*f.member);
		this->*f.member = obs_data_get_bool(root, f.file);
	}
	for (const AdvancedStringField &f : kAdvancedStringFields) {
		obs_data_set_default_string(root, f.file, (this->*f.member).c_str());
		this->*f.member = obs_data_get_string(root, f.file);
	}
	for (const AdvancedUIntField &f : kAdvancedUIntFields) {
		obs_data_set_default_int(root, f.file, this->*f.member);
		int64_t v = obs_data_get_int(root, f.file);
		v = v < (int64_t)f.min ? (int64_t)f.min : (v > (int64_t)f.max ? (int64_t)f.max : v);
		this->*f.member = (uint32_t)v;
	}
}

void AdvancedSettings::Save() const
{
	OBSDataAutoRelease root = obs_data_create();
	for (const AdvancedBoolField &f : kAdvancedBoolFields) {
		obs_data_set_bool(root, f.file, this->*f.member);
	}
	for (const AdvancedStringField &f : kAdvancedStringFields) {
		obs_data_set_string(root, f.file, (this->*f.member).c_str());
	}
	for (const AdvancedUIntField &f : kAdvancedUIntFields) {
		obs_data_set_int(root, f.file, this->*f.member);
	}

	SaveJsonAtomic(root, MultistreamBasicPath("advanced.json"));
}

void ApplyProcessPriority(const std::string &token)
{
#ifdef _WIN32
	static const struct {
		const char *token;
		DWORD cls;
	} kClasses[] = {
		{"normal", NORMAL_PRIORITY_CLASS},
		{"aboveNormal", ABOVE_NORMAL_PRIORITY_CLASS},
		{"high", HIGH_PRIORITY_CLASS},
	};
	for (const auto &c : kClasses) {
		if (token == c.token) {
			SetPriorityClass(GetCurrentProcess(), c.cls);
			return;
		}
	}
#else
	(void)token;
#endif
}
