#include "StorePaths.hpp"

#include <obs.h>
#include <obs.hpp>
#include <util/platform.h>

#include <nlohmann/json.hpp>

#include <filesystem>

#include "../log.hpp"
#include "../util/paths.hpp"

namespace {

// OBS-style portable marker: its mere presence next to the executable flips the
// config base to a self-contained "config" dir beside the exe, isolating a
// portable/dev build from an installed release that keeps using the per-user dir.
constexpr wchar_t kPortableMarker[] = L"braidcast_portable.txt";

std::string ResolveConfigBase()
{
	const std::wstring exeDir = ExecutableDir();
	if (!exeDir.empty() && MarkerFileBesideExe(kPortableMarker)) {
		const std::filesystem::path cfg = std::filesystem::path(exeDir) / L"config";
		os_mkdirs(cfg.generic_u8string().c_str());
		return cfg.generic_u8string();
	}
	char buf[512];
	if (os_get_config_path(buf, sizeof(buf), "braidcast") <= 0) {
		return std::string();
	}
	return std::string(buf);
}

nlohmann::json JsonFromData(obs_data_t *data)
{
	const char *js = data ? obs_data_get_json(data) : nullptr;
	return js ? nlohmann::json::parse(js) : nlohmann::json::object();
}

} // namespace

const std::string &BraidcastConfigDir()
{
	static const std::string base = ResolveConfigBase();
	return base;
}

std::string BraidcastConfigPath(const char *relative)
{
	const std::string &base = BraidcastConfigDir();
	if (base.empty()) {
		return std::string();
	}
	std::string path = base;
	path += "/";
	path += relative;
	return path;
}

bool MarkerFileBesideExe(const wchar_t *name)
{
	const std::wstring exeDir = ExecutableDir();
	if (exeDir.empty()) {
		return false;
	}
	std::error_code ec;
	return std::filesystem::exists(std::filesystem::path(exeDir) / name, ec);
}

bool SaveJsonAtomic(obs_data_t *root, const std::string &absPath)
{
	std::filesystem::path dir = std::filesystem::u8path(absPath).parent_path();
	if (!dir.empty()) {
		os_mkdirs(dir.u8string().c_str());
	}
	return obs_data_save_json_pretty_safe(root, absPath.c_str(), "tmp", "bak");
}

bool ReportSaveResult(bool saved, const std::string &path)
{
	if (!saved) {
		HostLog("[storage] failed to save " + path);
	}
	return saved;
}

nlohmann::json LoadStoreJson(const std::string &absPath)
{
	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(absPath.c_str(), "bak");
	return JsonFromData(root);
}

bool SaveStoreJson(const nlohmann::json &root, const std::string &absPath)
{
	OBSDataAutoRelease data = obs_data_create_from_json(root.dump().c_str());
	return ReportSaveResult(SaveJsonAtomic(data, absPath), absPath);
}

nlohmann::json StoreJsonFromArray(const char *key, obs_data_array_t *arr)
{
	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_array(root, key, arr);
	return JsonFromData(root);
}

obs_data_array_t *StoreArrayFromJson(const nlohmann::json &root, const char *key)
{
	if (!root.is_object()) {
		return nullptr;
	}
	OBSDataAutoRelease data = obs_data_create_from_json(root.dump().c_str());
	return data ? obs_data_get_array(data, key) : nullptr;
}
