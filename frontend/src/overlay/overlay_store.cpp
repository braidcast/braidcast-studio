#include "overlay_store.hpp"

#include "../log.hpp"
#include "../multistream/StorePaths.hpp"
#include "util/file_util.hpp"
#include "util/paths.hpp"
#include "util/random_util.hpp"
#include "uuid_util.hpp"

#include <obs.hpp>
#include <util/platform.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace Overlay {

namespace {

// Widget-token length; the server compares the whole string, so this is the only
// place it is decided.
constexpr size_t kTokenBytes = 16;

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

json Widget::ToJson() const
{
	return json{
		{"id", id},   {"token", token}, {"name", name},     {"type", type},     {"html", html},
		{"css", css}, {"js", js},       {"fields", fields}, {"assets", assets}, {"rev", rev},
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
	w.html = j.value("html", std::string());
	w.css = j.value("css", std::string());
	w.js = j.value("js", std::string());
	w.fields = j.contains("fields") && j["fields"].is_array() ? j["fields"] : json::array();
	w.assets = j.contains("assets") && j["assets"].is_array() ? j["assets"] : json::array();
	w.rev = j.value("rev", 0);
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

std::optional<Widget> OverlayStore::Create(const std::string &name, const std::string &type)
{
	Widget w;
	w.token = RandomUtil::HexToken(kTokenBytes);
	if (w.token.empty()) {
		HostLog("[overlay] Create: no system entropy for an access token; no widget created");
		return std::nullopt;
	}
	w.id = UuidUtil::New();
	w.name = name;
	w.type = type;

	// Seed html/css/js/fields from the on-disk default template for this type.
	const std::string dir = RundirRoot() + "/data/braidcast/web/overlay/default-" + type + "/";
	const bool haveHtml = FileUtil::ReadUtf8File(dir + "template.html", w.html);
	FileUtil::ReadUtf8File(dir + "template.css", w.css);
	FileUtil::ReadUtf8File(dir + "template.js", w.js);
	std::string fieldsJson;
	if (FileUtil::ReadUtf8File(dir + "fields.json", fieldsJson)) {
		try {
			json parsed = json::parse(fieldsJson);
			if (parsed.is_array()) {
				for (json field : parsed) {
					if (field.is_object() && !field.contains("value")) {
						field["value"] = field.value("default", json(nullptr));
					}
					w.fields.push_back(field);
				}
			}
		} catch (const std::exception &e) {
			HostLog("[overlay] Create: fields.json for type '" + type + "' is unparseable (" + e.what() +
				"); seeding the widget with no fields");
			w.fields = json::array();
		}
	}
	if (!haveHtml && w.fields.empty()) {
		HostLog("[overlay] Create: no template for type '" + type + "' at " + dir + " -- seeding empty widget");
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
		if (patch.contains("html") && patch["html"].is_string()) {
			w.html = patch["html"].get<std::string>();
		}
		if (patch.contains("css") && patch["css"].is_string()) {
			w.css = patch["css"].get<std::string>();
		}
		if (patch.contains("js") && patch["js"].is_string()) {
			w.js = patch["js"].get<std::string>();
		}
		if (patch.contains("fields") && patch["fields"].is_array()) {
			w.fields = patch["fields"];
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
			HostLog("[overlay] Duplicate: no such overlay " + id);
			return std::nullopt;
		}
		copy = *source;
		copy.token = RandomUtil::HexToken(kTokenBytes);
		if (copy.token.empty()) {
			HostLog("[overlay] Duplicate: no system entropy for an access token; no duplicate created");
			return std::nullopt;
		}
		copy.id = UuidUtil::New();
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
		HostLog("[overlay] duplicate of " + id + " aborted; the source was deleted mid-copy");
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

	// The server refuses a request whose token is empty, so a stored widget without one
	// is unreachable forever unless it is re-minted here. Its URL changes, which is why
	// the log names the widget.
	bool reminted = false;
	for (Widget &w : widgets_) {
		if (!w.token.empty()) {
			continue;
		}
		w.token = RandomUtil::HexToken(kTokenBytes);
		if (w.token.empty()) {
			HostLog("[overlay] widget " + w.id + " has no access token and none could be minted");
			continue;
		}
		reminted = true;
		HostLog("[overlay] widget " + w.id + " had no access token; minted one -- its URL changed");
	}
	if (reminted) {
		Save();
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
