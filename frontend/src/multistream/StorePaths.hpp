#pragma once

#include <util/platform.h>

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

// Absolute path to the Braidcast config base -- the parent of every state file
// the app reads/writes. Portable mode: when a portable marker file sits next to
// the executable, this is "<exe_dir>/config" (a self-contained dir beside the
// exe); otherwise it is the per-user config dir (os_get_config_path("braidcast"),
// i.e. %APPDATA%/braidcast on Windows). Resolved once and cached so the
// single-instance guard and every store observe one consistent base. Empty only
// when the platform config path can't be resolved.
const std::string &BraidcastConfigDir();

// Join `relative` (e.g. "basic/canvases.json", "oauth_tokens.json", "logs")
// under BraidcastConfigDir(). The single path seam every Braidcast store routes
// through, so portable-mode redirection lives in exactly one place.
std::string BraidcastConfigPath(const char *relative);

// CEF's own debug log, under the config base (BraidcastConfigPath). Where the CEF
// GPU-process crash signature lands.
inline constexpr char kCefDebugLogFile[] = "cef_debug.log";

// True when a marker file of this name sits next to the executable. The OBS-style
// portable-marker idiom -- a file's mere presence beside the exe flips a mode --
// shared by the portable-config marker here and the GPU-disable override marker in
// gpu_safe_mode. Wide name so it feeds std::filesystem directly.
bool MarkerFileBesideExe(const wchar_t *name);

// Resolves a file under the Braidcast "basic" state dir (<config base>/basic) --
// the SAME files the legacy Qt frontend wrote as userProfilesLocation +
// "/braidcast/basic/<file>". Routed through BraidcastConfigPath so it stays
// byte-identical in the default (non-portable) case yet follows portable mode
// with every other store.
inline std::string MultistreamBasicPath(const char *file)
{
	std::string rel = "basic/";
	rel += file;
	return BraidcastConfigPath(rel.c_str());
}

struct obs_data;
typedef struct obs_data obs_data_t;
struct obs_data_array;
typedef struct obs_data_array obs_data_array_t;

// Atomically persist `root` to the absolute `absPath`: create the parent
// directory, then obs_data_save_json_pretty_safe (write "<absPath>.tmp", rename
// into place, keep "<absPath>.bak"). Returns true on success. Centralizes the
// save envelope the multistream stores share.
bool SaveJsonAtomic(obs_data_t *root, const std::string &absPath);

// Log-and-forward the result of a SaveJsonAtomic (or any atomic save): on failure
// emit a "[storage] failed to save <path>" line so a disk-full/permission loss is
// never silent, then return `saved` unchanged so callers can propagate it. The one
// place every store routes its save result through, so the failure log reads
// identically wherever a session's edits are dropped.
bool ReportSaveResult(bool saved, const std::string &path);

// Read the state file at `absPath` (obs_data_create_from_json_file_safe, so a
// truncated write falls back to the ".bak" copy). Null when nothing usable came back;
// if the file or its ".bak" is on disk all the same, it is kept aside through
// KeepUnusableStore (logged under `tag`) and `unusable` reports that. The caller owns
// the returned reference.
[[nodiscard]] obs_data_t *LoadStoreData(const std::string &absPath, bool *unusable = nullptr,
					const char *tag = "[storage]");

// What LoadStoreJson does with a file it found but could not use. Leave is for a store
// under a secret, retention or purge rule, which a kept copy would outlive: the file is
// only logged, and stays where it is.
enum class OnUnusable { Keep, Leave };

// LoadStoreData as JSON: an empty object when the file is missing, unusable, or holds
// nothing -- so a caller's FromJson always sees a well-formed envelope. `unusable`
// reports a file that is on disk but could not be used, kept aside unless `onUnusable`
// is Leave.
nlohmann::json LoadStoreJson(const std::string &absPath, OnUnusable onUnusable = OnUnusable::Keep,
			     bool *unusable = nullptr);

// Persist `root` to `absPath` through SaveJsonAtomic and ReportSaveResult: the
// whole save envelope a store's Save() is, minus the model-to-JSON step.
bool SaveStoreJson(const nlohmann::json &root, const std::string &absPath);

// A store file the app found but could not use, kept beside the original so that a
// later save of the fallback cannot destroy it.
struct KeptStoreCopy {
	std::string name;   // the copy's file name; empty when neither file could be read
	bool fresh = false; // false when an identical earlier copy was reused
};

// Copy the store file at `absPath` beside it as <stem>.failed-<local time>.json
// ("canvases.json" -> "canvases.failed-..."), touching neither it nor its ".bak". The
// ".bak" is read only when the file itself can't be. A file that fails on every launch is
// copied once: an identical earlier copy is reported instead. nullopt only when the copy
// could not be written.
std::optional<KeptStoreCopy> KeepUnusableStoreFile(const std::string &absPath);

// The file-name prefix every copy KeepUnusableStoreFile makes of `absPath` starts with:
// "<stem>.failed-" ("canvases.json" -> "canvases.failed-").
std::string UnusableCopyPrefix(const std::string &absPath);

// Whether `fileName` is a copy KeepUnusableStoreFile made, so a directory scan for store
// files can pass over it.
bool IsUnusableStoreCopy(const std::string &fileName);

// For a load that got nothing usable from `absPath`. False on a first run (neither the
// file nor its ".bak" is on disk): the caller saves its defaults. Otherwise the file is
// kept through KeepUnusableStoreFile, the outcome is logged under `tag`, and the caller
// must not save the defaults it runs on over the file. Each store does that one of three
// ways:
//   - saving only from a user action (general, advanced, diagnostics, mcp.json, audio
//     devices, hotkeys, virtualcam, browser docks);
//   - an UnusableStoreHold, which skips saves of the untouched fallback (bindings, scene
//     links, stream meta, overlays, the scene collection);
//   - a gate on the load report for the whole session, where a pass would change the
//     fallback with no user action: CanvasStore::LoadedUnusable (its boot save, the
//     binding prune), transitions (Transitions::Init's save), and the boot target
//     reconcile on stream meta and bindings. The scheduled metadata save gates on the
//     stream meta hold still being armed instead, so its end-of-broadcast revert lands
//     once the user has saved a change of their own.
// One exception: the browser hardware-acceleration crash latch saves advanced.json at
// boot. Not for a store under a secret, retention or purge rule (events, chat, stream
// profiles): the copy would outlive it. Those load with OnUnusable::Leave and gate on the
// report themselves (StreamProfileStore holds, and the orphaned-account reclaim waits).
bool KeepUnusableStore(const std::string &absPath, const std::string &tag);

// Stops a store saving the fallback it runs on over its unusable file: that save would
// rotate the file into ".bak", and the next one would destroy it. Armed with the
// fallback's serialized form after a load that reported the file unusable; a save whose
// model still serializes to it is skipped, and the first one that differs (a change)
// releases it for good. A pass that would change the model with no user action is not
// covered, and gates on LoadedUnusable instead.
class UnusableStoreHold {
public:
	// After every load: armed with `fallback` when it reported the file unusable,
	// released otherwise.
	void AfterLoad(bool unusable, std::string fallback)
	{
		loadedUnusable_ = unusable;
		if (unusable) {
			fallback_ = std::move(fallback);
		} else {
			fallback_.reset();
		}
	}

	bool Armed() const { return fallback_.has_value(); }

	// Whether the last load reported its file unusable. Unlike Armed, a save does not lift
	// it: it lasts until the next load, i.e. for the session.
	bool LoadedUnusable() const { return loadedUnusable_; }

	// True when a save of a model serializing to `current` must be skipped.
	bool Skips(const std::string &current)
	{
		if (fallback_ && *fallback_ == current) {
			return true;
		}
		fallback_.reset();
		return false;
	}

private:
	std::optional<std::string> fallback_;
	bool loadedUnusable_ = false;
};

// Wrap `arr` as the sole `key` member of a JSON object -- the envelope shape every
// array-backed store file on disk holds. Does not take ownership of `arr`.
nlohmann::json StoreJsonFromArray(const char *key, obs_data_array_t *arr);

// The `key` array of a store envelope, or null when `root` is not an object or
// holds no such array (both of which a store treats as "nothing persisted").
// Caller owns the returned reference.
[[nodiscard]] obs_data_array_t *StoreArrayFromJson(const nlohmann::json &root, const char *key);

// Reorder `items` (move-only elements exposing a `.uuid`) to match `order`: for
// each uuid, move the first not-yet-moved match into place; unknown ids are
// ignored and any items absent from `order` keep their relative order at the end
// (repair). The single reorder algorithm the multistream stores share.
template<typename T> void ReorderByUuid(std::vector<T> &items, const std::vector<std::string> &order)
{
	std::vector<T> reordered;
	reordered.reserve(items.size());
	std::vector<bool> moved(items.size(), false);
	for (const std::string &uuid : order) {
		for (size_t i = 0; i < items.size(); i++) {
			if (!moved[i] && items[i].uuid == uuid) {
				reordered.push_back(std::move(items[i]));
				moved[i] = true;
				break;
			}
		}
	}
	for (size_t i = 0; i < items.size(); i++) {
		if (!moved[i]) {
			reordered.push_back(std::move(items[i]));
		}
	}
	items = std::move(reordered);
}
