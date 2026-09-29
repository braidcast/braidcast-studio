#include "DiagnosticsSettings.hpp"

#include "multistream/StorePaths.hpp"

#include <obs.hpp>

void DiagnosticsSettings::Load()
{
	// No file yet keeps the struct defaults, and so does one that is there but unusable,
	// which LoadStoreData has kept aside: the next save would rotate it into the .bak and
	// the one after would lose it.
	OBSDataAutoRelease root = LoadStoreData(MultistreamBasicPath("diagnostics.json"), nullptr, "[settings]");
	if (!root) {
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
