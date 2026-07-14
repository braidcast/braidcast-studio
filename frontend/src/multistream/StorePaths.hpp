#pragma once

#include <util/platform.h>

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

// Atomically persist `root` to the absolute `absPath`: create the parent
// directory, then obs_data_save_json_pretty_safe (write "<absPath>.tmp", rename
// into place, keep "<absPath>.bak"). Returns true on success. Centralizes the
// save envelope the multistream stores share.
bool SaveJsonAtomic(obs_data_t *root, const std::string &absPath);

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
