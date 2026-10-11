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
// A user plugin replaces a bundled module of the same name (compared case-insensitively):
// only the user's copy is opened, never both, because libobs would register the second
// copy's sources over the first's.
namespace ModulePlan {

// One entry directly under the user plugin root, as the scan found it. The scan reports
// facts; Plan() decides whether the entry has the shape the loader accepts.
struct UserEntry {
	// The entry's name as spelled on disk; for an accepted plugin, the module name.
	std::string name;
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
	SkipSmokeRun,
	SkipSelfTestRun,
};

enum class Origin { Bundled, User };

enum class Action {
	Load,
	SkipDenylist,
	SkipHelper,
	// A bundled module a same-name user plugin replaces.
	SkipOverridden,
	// A user plugin this launch does not open (UserPlugins other than Load).
	SkipUserPlugins,
	// A user root entry not in the <name>/bin/64bit/<name>.dll shape.
	IgnoreMalformed,
	// A user plugin whose name differs from an earlier one's only by case.
	IgnoreDuplicate,
};

struct Entry {
	std::string name;
	Origin origin = Origin::Bundled;
	Action action = Action::Load;
	// Empty for an ignored user entry, which never had a module path.
	std::string binPath;
	std::string dataPath;
	// The other half of an override: on the user plugin that loads, the bundled DLL it
	// replaces; on the bundled entry it replaces, the user DLL. Empty otherwise.
	std::string counterpartPath;
	// Why the entry got its action, worded for the log.
	std::string reason;
};

// The ordered plan. Bundled modules keep their scan order, and a user plugin that overrides
// one takes its slot, right after the bundled entry marked SkipOverridden. The remaining
// user entries follow in scan order. Every input appears exactly once, and at most one
// entry per lowercased module name has Action::Load. `denylist` and `nonModuleDlls` hold
// lowercased names and apply to both origins.
std::vector<Entry> Plan(const Inputs &in, const std::set<std::string> &denylist,
			const std::set<std::string> &nonModuleDlls, UserPlugins userPlugins);

} // namespace ModulePlan
