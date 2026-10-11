#pragma once

#include <set>
#include <string>
#include <vector>

// Which plugin DLL loads for each module name, decided apart from the disk and from libobs.
// The loader scans the bundled plugin dir and the user plugin root and hands in what it
// found; Plan() applies every rule, so a self-test can drive each one with synthetic input.
//
// User plugins use OBS's ProgramData layout, so an OBS plugin release unzips straight in:
// <user root>/<name>/bin/64bit/<name>.dll, with its data under <user root>/<name>/data/.
// A user plugin whose name is exactly a bundled module's replaces it: only the user's copy
// is opened, never both. libobs keeps the first registration of a source id and rejects
// later ones, so with both loaded whichever copy happened to load first would silently own
// every id the two share.
namespace ModulePlan {

// One entry directly under the user plugin root, as the scan found it. The scan reports
// facts; Plan() decides whether the entry has the shape the loader accepts.
struct UserEntry {
	// The entry's name as spelled on disk, in UTF-8; for an accepted plugin, the module name.
	std::string name;
	// False when the on-disk name does not survive conversion to UTF-8 (an unpaired UTF-16
	// surrogate), so no UTF-8 path could open it. `name` is then only fit for the log.
	bool validName = true;
	bool isDirectory = false;
	// Whether <root>/<name>/bin/64bit/<name>.dll is a regular file.
	bool hasModuleDll = false;
};

struct Inputs {
	// <rundir>/obs-plugins/64bit and <rundir>/data/obs-plugins, without a trailing slash.
	std::string bundledBinDir;
	std::string bundledDataRoot;
	// DLL file names found in bundledBinDir, in scan order. That order is the load order.
	std::vector<std::string> bundledDlls;
	// <config dir>/plugins, without a trailing slash.
	std::string userRoot;
	std::vector<UserEntry> userEntries;
};

// Whether this launch opens user plugins at all, and if not, why not.
enum class UserPlugins {
	Load,
	SkipSafeMode,
	SkipSelfTestRun,
};

// Why a launch does not open user plugins, worded for the log. Empty for Load.
const char *SkipReason(UserPlugins userPlugins);

enum class Origin { Bundled, User };

enum class Action {
	Load,
	SkipDenylist,
	SkipHelper,
	// A bundled module a same-name user plugin replaces.
	SkipOverridden,
	// A user plugin this launch does not open (UserPlugins other than Load).
	SkipUserPlugins,
	// A user plugin whose name matches a bundled module's only case-insensitively. libobs
	// names a module after its file name and looks names up case-sensitively, so loading it
	// in the bundled module's place would register the module under a different name. The
	// bundled module loads instead.
	SkipCaseMismatch,
	// A user root entry not in the <name>/bin/64bit/<name>.dll shape.
	IgnoreMalformed,
	// An entry whose name differs from an earlier one of the same origin only by case.
	IgnoreDuplicate,
};

struct Entry {
	std::string name;
	Origin origin = Origin::Bundled;
	Action action = Action::Load;
	// Empty for an ignored user entry, which never had a module path.
	std::string binPath;
	std::string dataPath;
	// The bundled module a user plugin is paired with: on the user plugin that loads, the
	// bundled DLL it replaces; on the bundled entry it replaces, the user DLL; on a
	// SkipCaseMismatch user plugin, the bundled DLL that loads instead. Empty otherwise.
	std::string counterpartPath;
	// Why the entry got its action, worded for the log.
	std::string reason;
};

// The ordered plan. Bundled modules keep their scan order, and a user plugin that overrides
// one takes its slot, right after the bundled entry marked SkipOverridden. Every other user
// entry follows all the bundled ones, in scan order, so a bundled module registers first
// and wins any id it shares with a user plugin that does not replace it. Every input
// appears exactly once, at most one entry per lowercased module name has Action::Load, and
// whatever loads under a bundled module's name has exactly that name. `denylist` and
// `nonModuleDlls` hold lowercased names and apply to both origins.
std::vector<Entry> Plan(const Inputs &in, const std::set<std::string> &denylist,
			const std::set<std::string> &nonModuleDlls, UserPlugins userPlugins);

} // namespace ModulePlan
