#include "module_plan.hpp"

#include <map>

#include "util/string_util.hpp"

namespace ModulePlan {

namespace {

std::string BaseNameNoExt(const std::string &filename)
{
	const size_t dot = filename.find_last_of('.');
	return dot == std::string::npos ? filename : filename.substr(0, dot);
}

const char *SkipUserPluginsReason(UserPlugins userPlugins)
{
	switch (userPlugins) {
	case UserPlugins::SkipSafeMode:
		return "safe mode";
	case UserPlugins::SkipSmokeRun:
		return "smoke run";
	case UserPlugins::SkipSelfTestRun:
		return "self-test run";
	case UserPlugins::Load:
		break;
	}
	return "";
}

// Applies the name-based skips shared by both origins. Returns false when the entry is
// skipped, with its action and reason set.
bool PassesNameFilters(Entry &e, const std::string &lname, const std::set<std::string> &denylist,
		       const std::set<std::string> &nonModuleDlls)
{
	if (denylist.count(lname)) {
		e.action = Action::SkipDenylist;
		e.reason = "denylist, Qt-coupled / no headless value";
		return false;
	}
	if (nonModuleDlls.count(lname)) {
		e.action = Action::SkipHelper;
		e.reason = "helper DLL, not a module";
		return false;
	}
	return true;
}

} // namespace

std::vector<Entry> Plan(const Inputs &in, const std::set<std::string> &denylist,
			const std::set<std::string> &nonModuleDlls, UserPlugins userPlugins)
{
	// Settle every user entry first, so the bundled pass can ask which names a user
	// plugin claims. `overrides` maps a lowercased name to the user plugin that would
	// load under it.
	std::vector<Entry> user;
	std::map<std::string, size_t> overrides;
	std::set<std::string> seen;
	for (const UserEntry &u : in.userEntries) {
		Entry e;
		e.name = u.name;
		e.origin = Origin::User;
		if (!u.isDirectory || !u.hasModuleDll) {
			e.action = Action::IgnoreMalformed;
			e.reason = u.isDirectory ? "no bin/64bit/" + u.name + ".dll inside"
						 : "not a directory; expected <name>/bin/64bit/<name>.dll";
			user.push_back(std::move(e));
			continue;
		}

		const std::string lname = StringUtil::ToLower(u.name);
		const std::string dir = in.userRoot + "/" + u.name;
		e.binPath = dir + "/bin/64bit/" + u.name + ".dll";
		e.dataPath = dir + "/data/";
		if (!seen.insert(lname).second) {
			e.action = Action::IgnoreDuplicate;
			e.reason = "an earlier user plugin has the same name";
		} else if (PassesNameFilters(e, lname, denylist, nonModuleDlls)) {
			if (userPlugins != UserPlugins::Load) {
				e.action = Action::SkipUserPlugins;
				e.reason = SkipUserPluginsReason(userPlugins);
			} else {
				e.action = Action::Load;
				overrides[lname] = user.size();
			}
		}
		user.push_back(std::move(e));
	}

	std::vector<Entry> plan;
	std::vector<bool> placed(user.size(), false);
	for (const std::string &file : in.bundledDlls) {
		Entry e;
		e.name = BaseNameNoExt(file);
		e.origin = Origin::Bundled;
		e.binPath = in.bundledBinDir + "/" + file;
		e.dataPath = in.bundledDataRoot + "/" + e.name + "/";

		const std::string lname = StringUtil::ToLower(e.name);
		if (!PassesNameFilters(e, lname, denylist, nonModuleDlls)) {
			plan.push_back(std::move(e));
			continue;
		}

		const auto twin = overrides.find(lname);
		if (twin == overrides.end()) {
			e.action = Action::Load;
			plan.push_back(std::move(e));
			continue;
		}

		Entry &replacement = user[twin->second];
		e.action = Action::SkipOverridden;
		e.reason = "replaced by a user plugin";
		e.counterpartPath = replacement.binPath;
		replacement.counterpartPath = e.binPath;
		replacement.reason = "overrides the bundled module";
		plan.push_back(std::move(e));
		plan.push_back(replacement);
		placed[twin->second] = true;
	}

	for (size_t i = 0; i < user.size(); ++i) {
		if (!placed[i]) {
			plan.push_back(std::move(user[i]));
		}
	}
	return plan;
}

} // namespace ModulePlan
