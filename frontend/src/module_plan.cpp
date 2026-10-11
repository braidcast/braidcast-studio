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

const char *kSameNameReason = "an earlier entry has the same name";

} // namespace

const char *SkipReason(UserPlugins userPlugins)
{
	switch (userPlugins) {
	case UserPlugins::SkipSafeMode:
		return "safe mode";
	case UserPlugins::SkipSelfTestRun:
		return "smoke or self-test run";
	case UserPlugins::Load:
		break;
	}
	return "";
}

std::vector<Entry> Plan(const Inputs &in, const std::set<std::string> &denylist,
			const std::set<std::string> &nonModuleDlls, UserPlugins userPlugins)
{
	// Settle every user entry first, so the bundled pass can ask which names a user
	// plugin claims. `claims` maps a lowercased name to the user plugin that would load
	// under it.
	std::vector<Entry> user;
	std::map<std::string, size_t> claims;
	std::set<std::string> userSeen;
	for (const UserEntry &u : in.userEntries) {
		Entry e;
		e.name = u.name;
		e.origin = Origin::User;
		if (!u.validName) {
			e.action = Action::IgnoreMalformed;
			e.reason = "name is not valid Unicode";
			user.push_back(std::move(e));
			continue;
		}
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
		if (!userSeen.insert(lname).second) {
			e.action = Action::IgnoreDuplicate;
			e.reason = kSameNameReason;
		} else if (PassesNameFilters(e, lname, denylist, nonModuleDlls)) {
			if (userPlugins != UserPlugins::Load) {
				e.action = Action::SkipUserPlugins;
				e.reason = SkipReason(userPlugins);
			} else {
				e.action = Action::Load;
				claims[lname] = user.size();
			}
		}
		user.push_back(std::move(e));
	}

	std::vector<Entry> plan;
	std::vector<bool> placed(user.size(), false);
	std::set<std::string> bundledSeen;
	for (const std::string &file : in.bundledDlls) {
		Entry e;
		e.name = BaseNameNoExt(file);
		e.origin = Origin::Bundled;
		e.binPath = in.bundledBinDir + "/" + file;
		e.dataPath = in.bundledDataRoot + "/" + e.name + "/";

		const std::string lname = StringUtil::ToLower(e.name);
		if (!bundledSeen.insert(lname).second) {
			e.action = Action::IgnoreDuplicate;
			e.reason = kSameNameReason;
			plan.push_back(std::move(e));
			continue;
		}
		if (!PassesNameFilters(e, lname, denylist, nonModuleDlls)) {
			plan.push_back(std::move(e));
			continue;
		}

		e.action = Action::Load;
		const auto claim = claims.find(lname);
		if (claim == claims.end()) {
			plan.push_back(std::move(e));
			continue;
		}

		Entry &replacement = user[claim->second];
		replacement.counterpartPath = e.binPath;
		if (replacement.name != e.name) {
			// Left unplaced, so it follows the bundled entries like any other user entry.
			replacement.action = Action::SkipCaseMismatch;
			replacement.reason = "its name differs from the bundled module " + e.name +
					     " only by case; rename the folder to " + e.name + " to override it";
			plan.push_back(std::move(e));
			continue;
		}

		e.action = Action::SkipOverridden;
		e.reason = "replaced by a user plugin";
		e.counterpartPath = replacement.binPath;
		replacement.reason = "overrides the bundled module";
		plan.push_back(std::move(e));
		plan.push_back(replacement);
		placed[claim->second] = true;
	}

	for (size_t i = 0; i < user.size(); ++i) {
		if (!placed[i]) {
			plan.push_back(std::move(user[i]));
		}
	}
	return plan;
}

} // namespace ModulePlan
