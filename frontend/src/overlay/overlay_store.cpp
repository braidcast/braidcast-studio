#include "overlay_store.hpp"

#include "../log.hpp"
#include "../multistream/StorePaths.hpp"
#include "util/paths.hpp"

#include <obs.hpp>
#include <util/platform.h>
#include <util/util.hpp>

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

#pragma comment(lib, "bcrypt.lib")

namespace Overlay {

namespace {

std::string NewUuid()
{
	BPtr<char> id = os_generate_uuid();
	return id ? std::string(id) : std::string();
}

std::string NewToken()
{
	unsigned char b[16];
	BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	static const char *hex = "0123456789abcdef";
	std::string s;
	s.reserve(32);
	for (unsigned char c : b) {
		s.push_back(hex[c >> 4]);
		s.push_back(hex[c & 15]);
	}
	return s;
}

// A widget's own directory (the parent of AssetsDir): the single place the
// overlays/<id> layout is spelled out.
std::string WidgetDir(const std::string &id)
{
	return MultistreamBasicPath(("overlays/" + id).c_str());
}

// Drop a widget's whole directory, errors swallowed (a leftover dir is not worth
// failing a delete over). Call with mutex_ RELEASED -- see the note in AddAsset.
void RemoveWidgetDir(const std::string &id)
{
	std::error_code ec;
	std::filesystem::remove_all(std::filesystem::u8path(WidgetDir(id)), ec);
}

// Clone one widget's assets directory onto another's. Call with mutex_ RELEASED -- the
// tree walk is the same blocking I/O RemoveWidgetDir and AddAsset keep off the lock. A
// source that has never had an upload has no directory, which is nothing to copy rather
// than a failure.
bool CopyAssetsDir(const std::string &fromId, const std::string &toId)
{
	const std::filesystem::path from = std::filesystem::u8path(OverlayStore::AssetsDir(fromId));
	std::error_code ec;
	if (!std::filesystem::exists(from, ec)) {
		return true;
	}
	const std::string to = OverlayStore::AssetsDir(toId);
	if (os_mkdirs(to.c_str()) == MKDIR_ERROR) {
		return false;
	}
	std::filesystem::copy(from, std::filesystem::u8path(to), std::filesystem::copy_options::recursive, ec);
	return !ec;
}

// The widget with this id, or null. Caller holds mutex_.
Widget *FindWidget(std::vector<Widget> &widgets, const std::string &id)
{
	for (Widget &w : widgets) {
		if (w.id == id) {
			return &w;
		}
	}
	return nullptr;
}

// Read a whole file (binary) into `out`; false if it can't be opened.
bool ReadWholeFile(const std::string &path, std::string &out)
{
	std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
	if (!f) {
		return false;
	}
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

// Reduce a caller-supplied asset key to a safe filename: keep only [A-Za-z0-9._-]
// (this already drops both path separators), then remove every ".." so a sanitized
// name can never traverse out of the assets dir. Empty result => caller must bail.
std::string SanitizeAssetKey(const std::string &key)
{
	std::string out;
	out.reserve(key.size());
	for (char c : key) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
				c == '.' || c == '_' || c == '-';
		if (ok) {
			out.push_back(c);
		}
	}
	size_t pos;
	while ((pos = out.find("..")) != std::string::npos) {
		out.erase(pos, 2);
	}
	return out;
}

} // namespace

const TypeTemplate &TypeTemplateFor(const std::string &type)
{
	static std::mutex cacheMutex;
	// Never erased, and std::map keeps references valid across later inserts, so the
	// reference handed out here stays good after the lock is dropped.
	static std::map<std::string, TypeTemplate> cache;

	std::lock_guard<std::mutex> lock(cacheMutex);
	const auto it = cache.find(type);
	if (it != cache.end()) {
		return it->second;
	}

	TypeTemplate t;
	const std::string dir = RundirRoot() + "/data/braidcast/web/overlay/default-" + type + "/";
	t.ok = ReadWholeFile(dir + "template.html", t.html);
	ReadWholeFile(dir + "template.css", t.css);
	ReadWholeFile(dir + "template.js", t.js);
	std::string schemaJson;
	if (ReadWholeFile(dir + "fields.json", schemaJson)) {
		try {
			json parsed = json::parse(schemaJson);
			if (parsed.is_array()) {
				t.schema = std::move(parsed);
			}
		} catch (const std::exception &e) {
			HostLog("[overlay] fields.json for type '" + type + "' is unparseable (" + e.what() +
				"); the type will offer no settings");
		}
	}
	if (!t.ok) {
		HostLog("[overlay] no template for type '" + type + "' at " + dir);
	}
	return cache.emplace(type, std::move(t)).first->second;
}

std::string Widget::Html() const
{
	return IsCustom() ? custom.value("html", std::string()) : TypeTemplateFor(type).html;
}

std::string Widget::Css() const
{
	return IsCustom() ? custom.value("css", std::string()) : TypeTemplateFor(type).css;
}

std::string Widget::Js() const
{
	return IsCustom() ? custom.value("js", std::string()) : TypeTemplateFor(type).js;
}

json Widget::FieldValues() const
{
	json out = json::object();
	// A fork carries its own field list, so its values come from there. `default` is the
	// fallback for a field the user declared but never gave a value.
	if (IsCustom()) {
		const json fields = custom.value("fields", json::array());
		for (const json &f : fields) {
			if (!f.is_object() || !f.contains("key") || !f["key"].is_string()) {
				continue;
			}
			out[f["key"].get<std::string>()] = f.contains("value") ? f["value"]
									       : f.value("default", json(nullptr));
		}
		return out;
	}
	// Stock: the schema decides which keys exist at all, so a setting left over from an
	// older template simply stops being served rather than leaking into the page.
	for (const json &f : TypeTemplateFor(type).schema) {
		if (!f.is_object() || !f.contains("key") || !f["key"].is_string()) {
			continue;
		}
		const std::string key = f["key"].get<std::string>();
		out[key] = settings.contains(key) ? settings[key] : f.value("default", json(nullptr));
	}
	return out;
}

json Widget::ToJson() const
{
	return json{
		{"id", id},   {"token", token},       {"name", name},     {"type", type},
		{"rev", rev}, {"settings", settings}, {"custom", custom}, {"assets", assets},
	};
}

json Widget::ToListJson(int port) const
{
	return json{{"id", id}, {"name", name}, {"type", type}, {"token", token}, {"url", WidgetUrl(*this, port)}};
}

Widget Widget::FromJson(const json &j)
{
	Widget w;
	if (!j.is_object()) {
		return w;
	}
	w.id = j.value("id", std::string());
	w.token = j.value("token", std::string());
	w.name = j.value("name", std::string());
	w.type = j.value("type", std::string());
	w.assets = j.contains("assets") && j["assets"].is_array() ? j["assets"] : json::array();
	w.rev = j.value("rev", 0);

	if (j.contains("settings") || j.contains("custom")) {
		w.settings = j.contains("settings") && j["settings"].is_object() ? j["settings"] : json::object();
		w.custom = j.contains("custom") && j["custom"].is_object() ? j["custom"] : json(nullptr);
		return w;
	}

	// Records written before widgets referenced their type's template each carried a
	// full copy of it. Whether the user ever edited that copy is not recorded, so the
	// only honest signal is the copy itself: identical to what this build ships means
	// untouched. A widget created against an OLDER template and never edited will not
	// match and is migrated as a fork -- wrong, but wrong in the safe direction, since
	// it keeps rendering exactly as it does today at the cost of no longer tracking the
	// template.
	const std::string html = j.value("html", std::string());
	const std::string css = j.value("css", std::string());
	const std::string js = j.value("js", std::string());
	const json fields = j.contains("fields") && j["fields"].is_array() ? j["fields"] : json::array();
	const TypeTemplate &t = TypeTemplateFor(w.type);

	if (!t.ok || html != t.html || css != t.css || js != t.js) {
		w.custom = json{{"html", html}, {"css", css}, {"js", js}, {"fields", fields}};
		return w;
	}

	// Untouched code: keep it stock, and lift across only the values that differ from
	// the schema default. Carrying the identical ones over would pin them, defeating
	// the point of storing overrides rather than a copy.
	for (const json &f : fields) {
		if (!f.is_object() || !f.contains("key") || !f["key"].is_string() || !f.contains("value")) {
			continue;
		}
		const std::string key = f["key"].get<std::string>();
		json fallback = json(nullptr);
		for (const json &s : t.schema) {
			if (s.is_object() && s.value("key", std::string()) == key) {
				fallback = s.value("default", json(nullptr));
				break;
			}
		}
		if (f["value"] != fallback) {
			w.settings[key] = f["value"];
		}
	}
	return w;
}

std::string WidgetUrl(const Widget &w, int port)
{
	return "http://127.0.0.1:" + std::to_string(port) + "/w/" + w.id + "?t=" + w.token;
}

std::string OverlayStore::FilePath()
{
	return MultistreamBasicPath("overlays.json");
}

std::string OverlayStore::AssetsDir(const std::string &id)
{
	return WidgetDir(id) + "/assets";
}

std::vector<Widget> OverlayStore::List() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return widgets_;
}

std::optional<Widget> OverlayStore::Get(const std::string &id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (const Widget &w : widgets_) {
		if (w.id == id) {
			return w;
		}
	}
	return std::nullopt;
}

int OverlayStore::Port() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return port_;
}

void OverlayStore::SetPort(int port)
{
	std::lock_guard<std::mutex> lock(mutex_);
	port_ = port;
	Save();
}

Widget OverlayStore::Create(const std::string &name, const std::string &type)
{
	Widget w;
	w.id = NewUuid();
	w.token = NewToken();
	w.name = name;
	w.type = type;

	// Nothing is copied: a new widget is stock, so it renders from its type's template
	// and every setting sits at that template's default until the user changes one.
	// Touch the template here anyway, so a missing one is reported at the moment the
	// user creates the widget rather than when a browser source first serves a blank.
	if (!TypeTemplateFor(type).ok) {
		HostLog("[overlay] Create: type '" + type + "' has no template; the widget will serve an empty page");
	}

	std::lock_guard<std::mutex> lock(mutex_);
	widgets_.push_back(w);
	Save();
	return w;
}

bool OverlayStore::Update(const std::string &id, const json &patch, int *newRev)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (Widget &w : widgets_) {
		if (w.id != id) {
			continue;
		}
		if (patch.contains("name") && patch["name"].is_string()) {
			w.name = patch["name"].get<std::string>();
		}
		// Replaced wholesale rather than merged. A setting back at its template default
		// is expressed by being ABSENT, so that it resumes tracking the default; merging
		// would make the removal unrepresentable and pin the value forever. The editor
		// holds the whole object, so it has nothing to gain from a partial patch.
		if (patch.contains("settings") && patch["settings"].is_object()) {
			w.settings = patch["settings"];
		}
		// Forking (object) and reverting to stock (null) are the same patch key, so the
		// editor's Custom Code toggle is one call in both directions. Anything else --
		// including a partial object -- is ignored rather than half-applied.
		if (patch.contains("custom")) {
			if (patch["custom"].is_null()) {
				w.custom = json(nullptr);
			} else if (patch["custom"].is_object()) {
				// Merge so an edit to one editor pane does not blank the other two.
				if (!w.IsCustom()) {
					w.custom = json::object();
				}
				for (const auto &[key, value] : patch["custom"].items()) {
					w.custom[key] = value;
				}
			}
		}
		++w.rev;
		if (newRev != nullptr) {
			*newRev = w.rev;
		}
		Save();
		return true;
	}
	return false;
}

bool OverlayStore::SetCustom(const std::string &id, bool enabled, int *newRev)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (Widget &w : widgets_) {
		if (w.id != id) {
			continue;
		}
		if (!enabled) {
			w.custom = json(nullptr);
		} else if (!w.IsCustom()) {
			const TypeTemplate &t = TypeTemplateFor(w.type);
			// The fork's field list is the type's schema with this widget's current
			// values baked in, so the editor opens on exactly what the user was already
			// looking at rather than on the type's defaults.
			json fields = json::array();
			for (json f : t.schema) {
				if (!f.is_object() || !f.contains("key") || !f["key"].is_string()) {
					continue;
				}
				const std::string key = f["key"].get<std::string>();
				f["value"] = w.settings.contains(key) ? w.settings[key]
								      : f.value("default", json(nullptr));
				fields.push_back(std::move(f));
			}
			w.custom = json{{"html", t.html}, {"css", t.css}, {"js", t.js}, {"fields", std::move(fields)}};
		}
		++w.rev;
		if (newRev != nullptr) {
			*newRev = w.rev;
		}
		Save();
		return true;
	}
	return false;
}

std::optional<Widget> OverlayStore::Duplicate(const std::string &id)
{
	Widget copy;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const Widget *source = FindWidget(widgets_, id);
		if (source == nullptr) {
			return std::nullopt;
		}
		copy = *source;
		copy.id = NewUuid();
		copy.token = NewToken();
		copy.name = source->name + " copy";
	}
	// Outside mutex_, on the same discipline AddAsset documents: this walks a directory
	// tree, and the video thread takes this lock on every overlay source's update.
	//
	// The copy keeps assets[], so every field holding an "assets/<file>" path resolves
	// under the new id. A failed copy fails the whole duplicate rather than registering a
	// widget whose alert sound and image 404 -- that failure mode is silent on stream,
	// which is the worst place to discover it.
	if (!CopyAssetsDir(id, copy.id)) {
		RemoveWidgetDir(copy.id);
		HostLog("[overlay] Duplicate: could not copy the assets of " + id + "; no duplicate created");
		return std::nullopt;
	}
	bool sourceGone = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (FindWidget(widgets_, id) != nullptr) {
			widgets_.push_back(copy);
			Save();
		} else {
			sourceGone = true;
		}
	}
	if (sourceGone) {
		// Deleted while the tree was being copied. Registering the copy now would put the
		// content that delete just removed straight back.
		RemoveWidgetDir(copy.id);
		return std::nullopt;
	}
	return copy;
}

bool OverlayStore::Delete(const std::string &id)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const size_t before = widgets_.size();
		widgets_.erase(std::remove_if(widgets_.begin(), widgets_.end(),
					      [&](const Widget &w) { return w.id == id; }),
			       widgets_.end());
		if (widgets_.size() == before) {
			return false;
		}
		Save();
	}
	// Outside mutex_: remove_all walks a directory tree, and the video thread now takes
	// this lock on every overlay source's update, so a held lock here is dropped frames.
	// The path derives from the id alone, so nothing under the lock is needed. Dropping
	// the directory AFTER the registry is persisted also fails safe -- a crash in
	// between orphans a directory nothing references, rather than leaving a widget whose
	// assets are already gone.
	RemoveWidgetDir(id);
	return true;
}

std::string OverlayStore::AddAsset(const std::string &id, const std::string &key, const std::string &kind,
				   const std::vector<unsigned char> &bytes)
{
	const std::string safeKey = SanitizeAssetKey(key);
	if (safeKey.empty()) {
		return std::string();
	}
	{
		// Existence check only; the blob below is written with mutex_ RELEASED. At the
		// 8 MB cap the write plus the WRITE_THROUGH move is tens of milliseconds, and
		// the video thread now takes this lock on every overlay source's update -- a
		// hold that long is several missed composites, not jitter. Same discipline
		// OverlayServer applies to sseMutex_ (snapshot under lock, do the blocking part
		// unlocked; see overlay_server.hpp).
		std::lock_guard<std::mutex> lock(mutex_);
		if (!FindWidget(widgets_, id)) {
			return std::string();
		}
	}

	const std::string dir = AssetsDir(id);
	if (os_mkdirs(dir.c_str()) == MKDIR_ERROR) {
		return std::string();
	}
	const std::string full = dir + "/" + safeKey;
	const std::filesystem::path fullPath = std::filesystem::u8path(full);
	const std::filesystem::path tmpPath = std::filesystem::u8path(full + ".tmp");
	// Atomic write: a crash or partial write must never leave a truncated asset the
	// overlay would then serve. Write the whole blob to a sibling temp, then atomically
	// replace the real file (mirrors OAuth::TokenStore::SaveLocked).
	{
		std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
		if (!out) {
			return std::string();
		}
		if (!bytes.empty()) {
			out.write(reinterpret_cast<const char *>(bytes.data()),
				  static_cast<std::streamsize>(bytes.size()));
		}
		out.flush();
		if (!out) {
			std::error_code ec;
			std::filesystem::remove(tmpPath, ec);
			return std::string();
		}
	}
	// MOVEFILE_REPLACE_EXISTING handles the first-write case too (dst absent -> plain
	// rename); MOVEFILE_WRITE_THROUGH flushes the metadata to disk.
	if (!MoveFileExW(tmpPath.c_str(), fullPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		std::error_code ec;
		std::filesystem::remove(tmpPath, ec);
		return std::string();
	}
	bool stillPresent = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		Widget *target = FindWidget(widgets_, id);
		if (target) {
			stillPresent = true;
			bool exists = false;
			for (const json &a : target->assets) {
				if (a.is_object() && a.value("file", std::string()) == safeKey) {
					exists = true;
					break;
				}
			}
			if (!exists) {
				target->assets.push_back(json{{"key", key}, {"kind", kind}, {"file", safeKey}});
			}
			Save();
		}
	}
	if (!stillPresent) {
		// Deleted while the blob was being written: that Delete already dropped the
		// directory, which the write above then recreated. Clear it again rather than
		// leave a directory no widget references.
		RemoveWidgetDir(id);
		return std::string();
	}
	return "assets/" + safeKey;
}

bool OverlayStore::RemoveAsset(const std::string &id, const std::string &file)
{
	// The same guard AddAsset writes through, so the two agree on what a stored file can
	// be named and a crafted `file` cannot reach outside the assets directory.
	const std::string safeFile = SanitizeAssetKey(file);
	if (safeFile.empty()) {
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		Widget *target = FindWidget(widgets_, id);
		if (target == nullptr) {
			return false;
		}
		json kept = json::array();
		bool found = false;
		for (const json &a : target->assets) {
			if (a.is_object() && a.value("file", std::string()) == safeFile) {
				found = true;
				continue;
			}
			kept.push_back(a);
		}
		if (!found) {
			return false;
		}
		target->assets = std::move(kept);
		Save();
	}
	// Outside mutex_, and only once the registry no longer names the file -- the ordering
	// Delete uses. A crash in between orphans a file nothing points at, rather than
	// leaving a record whose file is already gone.
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(AssetsDir(id) + "/" + safeFile), ec);
	return true;
}

void OverlayStore::InjectForTest(const Widget &w)
{
	std::lock_guard<std::mutex> lock(mutex_);
	widgets_.push_back(w);
}

void OverlayStore::RemoveForTest(const std::string &id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	widgets_.erase(std::remove_if(widgets_.begin(), widgets_.end(), [&](const Widget &w) { return w.id == id; }),
		       widgets_.end());
}

void OverlayStore::Load()
{
	// Called from the ctor before `this` is visible to any other thread, so no lock.
	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(FilePath().c_str(), "bak");
	const char *js = root ? obs_data_get_json(root) : nullptr;
	if (!js) {
		return;
	}
	json parsed;
	try {
		parsed = json::parse(js);
	} catch (const std::exception &e) {
		// A corrupt file starts the store empty rather than aborting boot, but the lost
		// widgets must not read as "none configured".
		HostLog(std::string("[overlay] overlays.json unparseable (") + e.what() +
			"); starting with no widgets");
		return;
	}
	if (!parsed.is_object()) {
		return;
	}
	port_ = parsed.value("port", 43000);
	if (parsed.contains("widgets") && parsed["widgets"].is_array()) {
		for (const json &item : parsed["widgets"]) {
			widgets_.push_back(Widget::FromJson(item));
		}
	}
}

void OverlayStore::Save() const
{
	json arr = json::array();
	for (const Widget &w : widgets_) {
		arr.push_back(w.ToJson());
	}
	json root = json{{"port", port_}, {"widgets", std::move(arr)}};

	OBSDataAutoRelease data = obs_data_create_from_json(root.dump().c_str());
	const std::string path = FilePath();
	ReportSaveResult(SaveJsonAtomic(data, path), path);
}

OverlayStore &Store()
{
	static OverlayStore s;
	return s;
}

} // namespace Overlay
