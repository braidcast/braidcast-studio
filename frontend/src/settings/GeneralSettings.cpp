#include "GeneralSettings.hpp"

#include "multistream/StorePaths.hpp"

#include <obs.hpp>
#include <util/platform.h>

void GeneralSettings::Load()
{
	OBSDataAutoRelease root =
		obs_data_create_from_json_file_safe(MultistreamBasicPath("general.json").c_str(), "bak");
	if (!root) {
		return; // no file yet: keep struct defaults
	}
	for (const GeneralBoolField &f : kGeneralBoolFields) {
		obs_data_set_default_bool(root, f.file, this->*f.member);
		this->*f.member = obs_data_get_bool(root, f.file);
	}
	for (const GeneralStringField &f : kGeneralStringFields) {
		obs_data_set_default_string(root, f.file, (this->*f.member).c_str());
		this->*f.member = obs_data_get_string(root, f.file);
	}
	for (const GeneralDoubleField &f : kGeneralDoubleFields) {
		obs_data_set_default_double(root, f.file, this->*f.member);
		double v = obs_data_get_double(root, f.file);
		v = v < f.min ? f.min : (v > f.max ? f.max : v);
		this->*f.member = v;
	}
}

void GeneralSettings::Save() const
{
	OBSDataAutoRelease root = obs_data_create();
	for (const GeneralBoolField &f : kGeneralBoolFields) {
		obs_data_set_bool(root, f.file, this->*f.member);
	}
	for (const GeneralStringField &f : kGeneralStringFields) {
		obs_data_set_string(root, f.file, (this->*f.member).c_str());
	}
	for (const GeneralDoubleField &f : kGeneralDoubleFields) {
		obs_data_set_double(root, f.file, this->*f.member);
	}

	SaveJsonAtomic(root, MultistreamBasicPath("general.json"));
}
