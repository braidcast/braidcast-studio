#include "StorePaths.hpp"

#include <obs.h>
#include <obs.hpp>
#include <util/platform.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

#include "../log.hpp"
#include "../util/env_config.hpp"
#include "../util/file_util.hpp"
#include "../util/paths.hpp"
#include "../util/time_util.hpp"

namespace {

// OBS-style portable marker: its mere presence next to the executable flips the
// config base to a self-contained "config" dir beside the exe, isolating a
// portable/dev build from an installed release that keeps using the per-user dir.
constexpr wchar_t kPortableMarker[] = L"braidcast_portable.txt";

// Where an unattended run keeps its state, and the file that marks the directory
// as ours to delete.
constexpr wchar_t kSelfTestConfigDir[] = L"config-selftest";
constexpr wchar_t kSelfTestMarker[] = L"braidcast_selftest_config.txt";

// A self-test run gets its own config base beside the executable, wiped at
// launch. Two reasons, and the second is the one that matters.
//
// The dev rundir's "config" is deliberately a junction onto the per-user
// directory, so an ordinary launch shares one data dir with an installed build.
// That means the portable branch below resolves onto the developer's live files,
// and the self-tests were editing them -- a smoke run left a capture behind on
// audio channel 6 and removed scene links rather than restoring them, and its
// session log evicted a real one from the ten the directory keeps. A suite must
// not be able to damage the thing it is testing.
//
// And a suite that runs against whatever scenes the developer happens to have is
// not a regression test. Several self-tests assert on the default scene the
// bootstrap builds, which any populated config replaces, so they were failing for
// reasons that said nothing about the code. A clean slate makes the run
// deterministic and its failures worth reading.
//
// The directory is left in place afterwards so a failed run can be inspected.
std::string ResolveSelfTestConfigBase()
{
	const std::wstring exeDir = ExecutableDir();
	if (exeDir.empty()) {
		return std::string();
	}

	const std::filesystem::path cfg = std::filesystem::path(exeDir) / kSelfTestConfigDir;
	std::error_code ec;

	// Only ever delete a directory this function created; the marker inside is
	// what says it did. A directory of that name from anywhere else is reused
	// rather than removed, which is the safe answer rather than the tidy one.
	if (std::filesystem::exists(cfg / kSelfTestMarker, ec)) {
		std::filesystem::remove_all(cfg, ec);
	}

	os_mkdirs(cfg.generic_u8string().c_str());
	std::ofstream(cfg / kSelfTestMarker) << "Braidcast self-test config. Deleted and recreated on every "
						"unattended run; nothing here is worth keeping.";

	return cfg.generic_u8string();
}

std::string ResolveConfigBase()
{
	if (Env::IsSelfTestRun()) {
		return ResolveSelfTestConfigBase();
	}

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

// Names a copy KeepUnusableStoreFile made: <stem>.failed-<local time>.json.
constexpr char kUnusableCopyInfix[] = ".failed-";

// Whether the store file at `absPath` or its ".bak" is on disk, i.e. whether a load that
// got nothing usable found something rather than a first run.
bool StoreFileOnDisk(const std::string &absPath)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	return !absPath.empty() &&
	       (fs::exists(fs::u8path(absPath), ec) || fs::exists(fs::u8path(absPath + ".bak"), ec));
}

// The one "<file> could not be used" line, so a Keep and a Leave store read alike in the log.
void LogUnusableStore(const std::string &tag, const std::string &absPath, const std::string &outcome)
{
	HostLog(tag + " " + std::filesystem::u8path(absPath).filename().u8string() +
		" could not be used; running on defaults (" + outcome + ")");
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

namespace {

// Whether the file at `path` loads: obs_data's own parser (jansson, duplicate keys refused)
// accepts it, as it must for the store's load. A temp file a write abandoned midway does not,
// so this tells a finished one apart. nlohmann looks first so that text which is not JSON at
// all never reaches jansson, whose error line quotes the text it stopped at.
bool LoadsAsObsData(const std::filesystem::path &path)
{
	std::string text;
	if (!FileUtil::ReadBinaryFile(path, text) || !nlohmann::json::accept(text)) {
		return false;
	}
	OBSDataAutoRelease data = obs_data_create_from_json(text.c_str());
	return data != nullptr;
}

// Make "<absPath>.bak" a copy of the store file, or remove it when the copy fails. A backup
// that can be neither is logged; the next save or load of the store tries again.
void ReplaceBackupWithCurrent(const std::string &absPath)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path bak = fs::u8path(absPath + ".bak");
	if (fs::copy_file(fs::u8path(absPath), bak, fs::copy_options::overwrite_existing, ec)) {
		return;
	}
	ec.clear();
	if (!fs::remove(bak, ec) && ec) {
		HostLog("[storage] the backup of " + absPath +
			" still holds an older state; cleared at the next save or load");
	}
}

} // namespace

bool DropStoreHistory(const std::string &absPath)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path file = fs::u8path(absPath);
	const fs::path tmp = fs::u8path(absPath + ".tmp");
	bool promoted = false;
	if (!fs::exists(file, ec)) {
		// A replace that gave up after moving the old file into ".bak" leaves the new state
		// only in the temp file: that is the newest state there is, so it goes into place.
		// A temp file that does not load is an abandoned write and stays where it is.
		if (!LoadsAsObsData(tmp)) {
			return false;
		}
		fs::rename(tmp, file, ec);
		if (ec) {
			HostLog("[storage] " + absPath +
				" is missing and its finished write could not be moved into place");
			return false;
		}
		promoted = true;
	}
	if (!LoadsAsObsData(file)) {
		return promoted; // unreadable: the backup is what a load falls back to
	}
	fs::remove(tmp, ec);
	ReplaceBackupWithCurrent(absPath);
	return promoted;
}

bool SaveJsonAtomicDroppingHistory(obs_data_t *root, const std::string &absPath)
{
	if (SaveJsonAtomic(root, absPath)) {
		// The save rotated the outgoing file into ".bak".
		ReplaceBackupWithCurrent(absPath);
		return true;
	}
	// The temp file DropStoreHistory may move into place can be an older save's, when this
	// one failed before writing its own: only one holding exactly what this save wrote puts
	// the new state in the file.
	const char *json = obs_data_get_json_pretty(root);
	std::string tmp;
	const bool tmpIsThisSave = json && FileUtil::ReadBinaryFile(std::filesystem::u8path(absPath + ".tmp"), tmp) &&
				   tmp == json;
	return DropStoreHistory(absPath) && tmpIsThisSave;
}

bool ReportSaveResult(bool saved, const std::string &path)
{
	if (!saved) {
		HostLog("[storage] failed to save " + path);
	}
	return saved;
}

std::optional<KeptStoreCopy> KeepUnusableStoreFile(const std::string &absPath)
{
	namespace fs = std::filesystem;
	const fs::path saved = fs::u8path(absPath);
	std::string bytes;
	if (!FileUtil::ReadBinaryFile(saved, bytes) && !FileUtil::ReadBinaryFile(fs::u8path(absPath + ".bak"), bytes)) {
		return KeptStoreCopy{};
	}

	const std::string prefix = UnusableCopyPrefix(absPath);
	std::error_code ec;
	for (fs::directory_iterator it(saved.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
		const std::string name = it->path().filename().u8string();
		std::string existing;
		if (name.rfind(prefix, 0) == 0 && FileUtil::ReadBinaryFile(it->path(), existing) && existing == bytes) {
			return KeptStoreCopy{name, false};
		}
	}

	const std::string name = prefix + TimeUtil::LocalFileStamp() + ".json";
	const fs::path target = saved.parent_path() / fs::u8path(name);
	std::ofstream out(target, std::ios::out | std::ios::binary);
	if (!(out << bytes) || !out.flush()) {
		// A truncated copy left behind would read as a faithful one to whoever finds it.
		out.close();
		fs::remove(target, ec);
		return std::nullopt;
	}
	return KeptStoreCopy{name, true};
}

std::string UnusableCopyPrefix(const std::string &absPath)
{
	// Store names hold no ".failed-" of their own (fixed names, and collection slugs have no
	// dots), so this prefix can only ever match copies of this one file.
	return std::filesystem::u8path(absPath).stem().u8string() + kUnusableCopyInfix;
}

bool IsUnusableStoreCopy(const std::string &fileName)
{
	return fileName.find(kUnusableCopyInfix) != std::string::npos;
}

bool KeepUnusableStore(const std::string &absPath, const std::string &tag)
{
	if (!StoreFileOnDisk(absPath)) {
		return false;
	}
	const std::optional<KeptStoreCopy> kept = KeepUnusableStoreFile(absPath);
	const std::string outcome = !kept                ? "a copy could not be written"
				    : kept->name.empty() ? "nothing in it could be read to keep"
				    : kept->fresh        ? "kept as " + kept->name
							 : "already kept as " + kept->name;
	LogUnusableStore(tag, absPath, outcome);
	return true;
}

obs_data_t *LoadStoreData(const std::string &absPath, bool *unusable, const char *tag)
{
	obs_data_t *root = absPath.empty() ? nullptr : obs_data_create_from_json_file_safe(absPath.c_str(), "bak");
	const bool kept = !root && KeepUnusableStore(absPath, tag);
	if (unusable) {
		*unusable = kept;
	}
	return root;
}

nlohmann::json LoadStoreJson(const std::string &absPath, OnUnusable onUnusable, bool *unusable)
{
	if (onUnusable == OnUnusable::Keep) {
		OBSDataAutoRelease root = LoadStoreData(absPath, unusable);
		return JsonFromData(root);
	}
	OBSDataAutoRelease root = absPath.empty() ? nullptr
						  : obs_data_create_from_json_file_safe(absPath.c_str(), "bak");
	const bool left = !root && StoreFileOnDisk(absPath);
	if (left) {
		LogUnusableStore("[storage]", absPath, "left in place, not copied");
	}
	if (unusable) {
		*unusable = left;
	}
	return JsonFromData(root);
}

bool SaveStoreJson(const nlohmann::json &root, const std::string &absPath, SaveHistory history)
{
	OBSDataAutoRelease data = obs_data_create_from_json(root.dump().c_str());
	const bool saved = history == SaveHistory::Drop ? SaveJsonAtomicDroppingHistory(data, absPath)
							: SaveJsonAtomic(data, absPath);
	return ReportSaveResult(saved, absPath);
}

bool OrderedStoreSave::Write(const nlohmann::json &root, const std::string &absPath, uint64_t stamp)
{
	std::lock_guard<std::mutex> lock(writeMutex_);
	if (stamp < written_) {
		return true; // a newer snapshot is already on disk
	}
	if (!SaveStoreJson(root, absPath, history_)) {
		return false;
	}
	written_ = stamp;
	return true;
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
