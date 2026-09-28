#include "DiagnosticsSettings.hpp"

#include "multistream/StorePaths.hpp"

#include <obs.hpp>

void DiagnosticsSettings::Load()
{
	const std::string path = MultistreamBasicPath("diagnostics.json");
	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!root) {
		// No file yet keeps the struct defaults. A file that is there but unusable is kept
		// aside first: the next save would rotate it into the .bak and the one after would
		// lose it.
		KeepUnusableStore(path, "diagnostics.failed-", "[settings]");
		return;
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
