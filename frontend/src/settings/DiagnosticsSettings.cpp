#include "DiagnosticsSettings.hpp"

#include "multistream/StorePaths.hpp"

#include <obs.hpp>

void DiagnosticsSettings::Load()
{
	OBSDataAutoRelease root =
		obs_data_create_from_json_file_safe(MultistreamBasicPath("diagnostics.json").c_str(), "bak");
	if (!root) {
		return; // no file yet: keep struct defaults
	}
	for (const DiagnosticsBoolField &f : kDiagnosticsBoolFields) {
		obs_data_set_default_bool(root, f.file, this->*f.member);
		this->*f.member = obs_data_get_bool(root, f.file);
	}
}

bool DiagnosticsSettings::Save() const
{
	OBSDataAutoRelease root = obs_data_create();
	for (const DiagnosticsBoolField &f : kDiagnosticsBoolFields) {
		obs_data_set_bool(root, f.file, this->*f.member);
	}

	const std::string path = MultistreamBasicPath("diagnostics.json");
	return ReportSaveResult(SaveJsonAtomic(root, path), path);
}
