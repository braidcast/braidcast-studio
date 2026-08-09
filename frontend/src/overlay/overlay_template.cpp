#include "overlay_template.hpp"

#include "../log.hpp"
#include "util/file_util.hpp"
#include "util/web_bundle.hpp"

#include <exception>
#include <map>
#include <mutex>
#include <utility>

namespace Overlay {

namespace {

std::mutex g_cacheMutex;
std::map<std::string, TypeTemplate> g_cache;

TypeTemplate ReadTemplate(const std::string &type)
{
	TypeTemplate t;
	const std::string dir = TemplateDir(type);
	std::string fieldsJson;
	// One row per file a template is made of, so "complete" is a list rather than four
	// hand-written reads and a conjunction someone has to remember to extend. Every
	// result is checked: a read whose outcome is discarded is a failure that reaches the
	// user as an empty stylesheet with nothing in the log.
	std::string unread;
	size_t read = 0;
	for (const auto &[name, out] : std::initializer_list<std::pair<const char *, std::string *>>{
		     {"template.html", &t.html},
		     {"template.css", &t.css},
		     {"template.js", &t.js},
		     {"fields.json", &fieldsJson},
	     }) {
		if (FileUtil::ReadUtf8File(dir + name, *out)) {
			++read;
			continue;
		}
		unread += unread.empty() ? "" : ", ";
		unread += name;
	}

	if (read == 0) {
		t.status = TemplateStatus::Absent;
		return t;
	}
	if (!unread.empty()) {
		// Named individually because the usual cause is one file, momentarily: knowing
		// which one is the difference between "a bad install dropped template.css" and
		// "an antivirus pass held it for a moment".
		HostLog("[overlay] template for type '" + type + "' at " + dir + " is incomplete (could not read " +
			unread + ")");
		t.status = TemplateStatus::Partial;
		return t;
	}

	json parsed;
	try {
		parsed = json::parse(fieldsJson);
	} catch (const std::exception &e) {
		HostLog("[overlay] fields.json for type '" + type + "' is unparseable (" + e.what() + ")");
		t.status = TemplateStatus::Corrupt;
		return t;
	}
	// A file that parses but is not an array carries no field list either, and it reaches
	// a caller as the same empty result an unparseable one would.
	if (!parsed.is_array()) {
		HostLog("[overlay] fields.json for type '" + type + "' is not an array");
		t.status = TemplateStatus::Corrupt;
		return t;
	}
	t.schema = std::move(parsed);
	t.status = TemplateStatus::Ok;
	return t;
}

} // namespace

std::string TemplateDir(const std::string &type)
{
	// Off WebBundle::Root() rather than a second spelling of the rundir layout: this is
	// the same tree scheme.cpp serves at app://app/, so the two cannot name different
	// directories after one of them moves.
	return WebBundle::Root() + "/overlay/default-" + type + "/";
}

TypeTemplate TemplateFor(const std::string &type)
{
	// Read under the lock so two threads asking for the same unread type do not both read
	// it. The read is four small files off the local rundir, and only the first ask per
	// type pays for it.
	std::lock_guard<std::mutex> lock(g_cacheMutex);
	const auto cached = g_cache.find(type);
	if (cached != g_cache.end()) {
		return cached->second;
	}
	TypeTemplate t = ReadTemplate(type);
	DBG(LogCat::Overlay, "template for type '%s' read (status=%d)", type.c_str(), static_cast<int>(t.status));
	if (t.status != TemplateStatus::Ok) {
		// Deliberately not kept. An incomplete or unparseable read is a fact about one
		// attempt, and caching it would turn a file locked for a moment into a type that
		// serves unstyled for the rest of the session -- and into an empty stylesheet
		// written permanently into the next fork of it.
		return t;
	}
	return g_cache.emplace(type, std::move(t)).first->second;
}

json SchemaDefault(const json &schema, const std::string &key)
{
	if (!schema.is_array()) {
		return json(nullptr);
	}
	for (const json &f : schema) {
		if (!f.is_object()) {
			continue;
		}
		const auto keyIt = f.find("key");
		if (keyIt != f.end() && keyIt->is_string() && keyIt->get<std::string>() == key) {
			return f.value("default", json(nullptr));
		}
	}
	return json(nullptr);
}

json MergeSettings(const json &schema, const json &settings)
{
	json out = json::object();
	if (!schema.is_array()) {
		return out;
	}
	for (const json &f : schema) {
		if (!f.is_object()) {
			continue;
		}
		const auto keyIt = f.find("key");
		if (keyIt == f.end() || !keyIt->is_string()) {
			continue;
		}
		const std::string key = keyIt->get<std::string>();
		// find() answers end() for any non-object, so a widget whose settings were
		// stored as something else simply has no overrides rather than throwing.
		const auto override_ = settings.find(key);
		out[key] = override_ != settings.end() ? *override_ : f.value("default", json(nullptr));
	}
	return out;
}

} // namespace Overlay
