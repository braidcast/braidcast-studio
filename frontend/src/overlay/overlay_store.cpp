#include "overlay_store.hpp"

#include "../log.hpp"
#include "../multistream/StorePaths.hpp"
#include "../paths.hpp"

#include <obs.hpp>
#include <util/platform.h>
#include <util/util.hpp>

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

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

json Widget::ToJson() const
{
	return json{
		{"id", id},     {"token", token}, {"name", name},     {"type", type},
		{"html", html}, {"css", css},     {"js", js},         {"fields", fields},
		{"assets", assets},
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
	return MultistreamBasicPath(("overlays/" + id + "/assets").c_str());
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

	// Seed html/css/js/fields from the on-disk default template for this type.
	const std::string dir = RundirRoot() + "/data/braidcast/web/overlay/default-" + type + "/";
	const bool haveHtml = ReadWholeFile(dir + "template.html", w.html);
	ReadWholeFile(dir + "template.css", w.css);
	ReadWholeFile(dir + "template.js", w.js);
	std::string fieldsJson;
	if (ReadWholeFile(dir + "fields.json", fieldsJson)) {
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
		} catch (...) {
			w.fields = json::array();
		}
	}
	if (!haveHtml && w.fields.empty()) {
		HostLog("[overlay] Create: no template for type '" + type + "' at " + dir +
			" -- seeding empty widget");
	}

	std::lock_guard<std::mutex> lock(mutex_);
	widgets_.push_back(w);
	Save();
	return w;
}

bool OverlayStore::Update(const std::string &id, const json &patch)
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
		Save();
		return true;
	}
	return false;
}

std::optional<Widget> OverlayStore::Duplicate(const std::string &id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (const Widget &w : widgets_) {
		if (w.id != id) {
			continue;
		}
		Widget copy = w;
		copy.id = NewUuid();
		copy.token = NewToken();
		copy.name = w.name + " copy";
		// v1: asset FILES are not copied; the duplicate references no assets until re-uploaded.
		copy.assets = json::array();
		widgets_.push_back(copy);
		Save();
		return copy;
	}
	return std::nullopt;
}

bool OverlayStore::Delete(const std::string &id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const size_t before = widgets_.size();
	widgets_.erase(std::remove_if(widgets_.begin(), widgets_.end(),
				      [&](const Widget &w) { return w.id == id; }),
		       widgets_.end());
	if (widgets_.size() == before) {
		return false;
	}
	// Remove the widget's whole overlays/<id> dir (the parent of AssetsDir).
	std::error_code ec;
	std::filesystem::remove_all(std::filesystem::u8path(MultistreamBasicPath(("overlays/" + id).c_str())), ec);
	Save();
	return true;
}

std::string OverlayStore::AddAsset(const std::string &id, const std::string &key, const std::string &kind,
				   const std::vector<unsigned char> &bytes)
{
	std::lock_guard<std::mutex> lock(mutex_);
	Widget *target = nullptr;
	for (Widget &w : widgets_) {
		if (w.id == id) {
			target = &w;
			break;
		}
	}
	if (!target) {
		return std::string();
	}
	const std::string safeKey = SanitizeAssetKey(key);
	if (safeKey.empty()) {
		return std::string();
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
	return "assets/" + safeKey;
}

void OverlayStore::InjectForTest(const Widget &w)
{
	std::lock_guard<std::mutex> lock(mutex_);
	widgets_.push_back(w);
}

void OverlayStore::RemoveForTest(const std::string &id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	widgets_.erase(std::remove_if(widgets_.begin(), widgets_.end(),
				      [&](const Widget &w) { return w.id == id; }),
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
	} catch (...) {
		return; // a corrupt file starts the store empty rather than aborting boot
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
	SaveJsonAtomic(data, FilePath());
}

OverlayStore &Store()
{
	static OverlayStore s;
	return s;
}

} // namespace Overlay
