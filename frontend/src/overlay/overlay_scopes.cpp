#include "overlay_scopes.hpp"

#include "../log.hpp"
#include "overlay_template.hpp"
#include "util/file_util.hpp"
#include "util/web_bundle.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

namespace Overlay {

namespace {

constexpr size_t kMaxLibraryPathBytes = 256;
constexpr int kMaxLibraryDepth = 2;
constexpr char kLibraryManifest[] = "sounds.json";

// The fields.json types whose value is an audio file the page plays. The editor's half of
// the same registry is FIELD_TYPES in frontend/web/src/lib/overlays/fieldTypes.ts; nothing
// links them, so a sound type added there must be added here too. Listing these URLs in the
// bootstrap is what lets the runtime decode a widget's sounds once at load rather than
// building, fetching and decoding a fresh media element per alert -- and it covers a FORKED
// widget for free, because a fork carries its own schema through the same Resolve().
constexpr const char *kSoundFieldTypes[] = {"sound-upload", "sound"};
constexpr char kAssetPrefix[] = "assets/";
constexpr char kLibraryPrefix[] = "library:";
// A field the schema marks `"scope": "widget"` exists only in Defaults (a burst setting);
// anything else may be set per event and per variation.
constexpr char kWidgetOnlyScope[] = "widget";

std::mutex g_manifestMutex;
// id -> file, once the manifest has read. Never invalidated: the library is staged into the
// rundir by the build, so it cannot change under a running app.
std::optional<std::map<std::string, std::string>> g_manifest;

bool IsLibraryNameChar(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
	       c == '-';
}

// The manifest as id -> file, or nullopt when it does not read or parse. An entry whose file
// would not pass IsSafeLibraryPath is left out, so a bad row cannot become a served URL.
std::optional<std::map<std::string, std::string>> ReadManifest()
{
	std::string text;
	if (!FileUtil::ReadUtf8File(LibraryRoot() + "/" + kLibraryManifest, text)) {
		return std::nullopt;
	}
	const json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
	if (!parsed.is_array()) {
		return std::nullopt;
	}
	std::map<std::string, std::string> out;
	for (const json &row : parsed) {
		if (!row.is_object()) {
			continue;
		}
		const std::string id = row.value("id", std::string());
		const std::string file = row.value("file", std::string());
		if (!id.empty() && IsSafeLibraryPath(file)) {
			out.emplace(id, file);
		}
	}
	return out;
}

// The schema's keys, sorted by what the served copy does with them.
struct SchemaKeys {
	std::set<std::string> sounds;   // the value is an audio file the page plays
	std::set<std::string> perAlert; // an event or variation may set it
};

SchemaKeys ReadSchemaKeys(const json &schema)
{
	SchemaKeys keys;
	if (!schema.is_array()) {
		return keys;
	}
	for (const json &f : schema) {
		if (!f.is_object() || !f.contains("key") || !f["key"].is_string()) {
			continue;
		}
		const std::string key = f["key"].get<std::string>();
		const std::string type = f.value("type", std::string());
		for (const char *soundType : kSoundFieldTypes) {
			if (type == soundType) {
				keys.sounds.insert(key);
			}
		}
		if (f.value("scope", std::string()) != kWidgetOnlyScope) {
			keys.perAlert.insert(key);
		}
	}
	return keys;
}

// Rewrite one value in place to the URL this server serves it at; true when it is now a
// sound URL the runtime may preload.
//
// An upload is stored as the portable, token-less "assets/<file>". The page gets the absolute
// tokenized URL (/w/<id>/assets/<file>?t=<token>&r=<rev>): a bare "assets/<file>" would
// resolve against /w/ (no <base>) and 404, and lacks the required token. Matched by prefix, so
// it works whatever the field's declared type.
//
// `r` is the widget revision, and it is what makes this URL safe to cache for a long time.
// AddAsset replaces an upload IN PLACE at the same filename, so without it a re-upload under
// the same name keeps the same URL and a browser source that reloaded would re-read its own
// still-fresh cache entry and play the OLD bytes. AddAsset is the only writer of those bytes
// and bumps the revision itself, under the same lock as the write, so that cannot happen:
// changing the bytes changes this URL. What this does NOT do is reload a source already on a
// scene -- overlays.uploadAsset sweeps nothing -- so that source keeps its old document and
// its old sound until something else reloads it.
//
// A library sound ("library:<id>") becomes /lib/<file> through the manifest, on a sound field
// only. An id the manifest does not list is left as it is, which the runtime reads as "no
// sound" and logs once.
//
// Only rewritten values are preloaded. A fork whose fields.json defaults a sound field to an
// absolute http(s) URL is left off the list: a media element plays a cross-origin sound
// without CORS, fetch does not, so preloading one would spend a request to earn a CORS
// failure on every page load and then play it correctly through the element anyway.
bool ServeValue(json &value, bool isSound, const Widget &w)
{
	if (!value.is_string()) {
		return false;
	}
	const std::string s = value.get<std::string>();
	if (s.rfind(kAssetPrefix, 0) == 0) {
		value = "/w/" + w.id + "/" + s + "?t=" + w.token + "&r=" + std::to_string(w.rev);
		return isSound;
	}
	if (isSound && s.rfind(kLibraryPrefix, 0) == 0) {
		if (const std::optional<std::string> file = LibraryFileFor(s.substr(sizeof(kLibraryPrefix) - 1))) {
			value = std::string(kLibraryRoutePrefix) + *file;
			return true;
		}
	}
	return false;
}

// Two fields -- or two scopes -- can name one sound; decoding it twice would only evict
// something else from the runtime's cache.
void AddSound(json &sounds, const json &url)
{
	if (std::find(sounds.begin(), sounds.end(), url) == sounds.end()) {
		sounds.push_back(url);
	}
}

// One scope layer as the page receives it: the keys an alert scope may set, values served,
// and every sound it names added to `sounds`.
json ServeLayer(const json &layer, const SchemaKeys &keys, const Widget &w, json &sounds)
{
	json out = json::object();
	if (!layer.is_object()) {
		return out;
	}
	for (auto it = layer.begin(); it != layer.end(); ++it) {
		if (keys.perAlert.count(it.key()) == 0) {
			continue;
		}
		json value = it.value();
		if (ServeValue(value, keys.sounds.count(it.key()) > 0, w)) {
			AddSound(sounds, value);
		}
		out[it.key()] = std::move(value);
	}
	return out;
}

} // namespace

std::string LibraryRoot()
{
	return WebBundle::Root() + "/overlay/library";
}

bool IsSafeLibraryPath(const std::string &rel)
{
	if (rel.empty() || rel.size() > kMaxLibraryPathBytes) {
		return false;
	}
	int depth = 0;
	size_t start = 0;
	while (start <= rel.size()) {
		const size_t slash = rel.find('/', start);
		const std::string segment =
			rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
		if (segment.empty() || segment.front() == '.' || ++depth > kMaxLibraryDepth) {
			return false;
		}
		for (char c : segment) {
			if (!IsLibraryNameChar(c)) {
				return false;
			}
		}
		if (slash == std::string::npos) {
			break;
		}
		start = slash + 1;
	}
	// The character rules already make an escape impossible; this is the belt to them, so
	// a later loosening of the rules cannot quietly serve a file from outside the library.
	std::error_code ec;
	const std::filesystem::path root =
		std::filesystem::weakly_canonical(std::filesystem::u8path(LibraryRoot()), ec);
	if (ec) {
		return false;
	}
	const std::filesystem::path full = std::filesystem::weakly_canonical(root / std::filesystem::u8path(rel), ec);
	if (ec) {
		return false;
	}
	const auto mismatch = std::mismatch(root.begin(), root.end(), full.begin(), full.end());
	return mismatch.first == root.end();
}

ServedData BuildServedData(const Widget &w, const ResolvedWidget &resolved)
{
	ServedData out;
	const SchemaKeys keys = ReadSchemaKeys(resolved.schema);
	out.fields = MergeSettings(resolved.schema, w.settings);
	for (auto it = out.fields.begin(); it != out.fields.end(); ++it) {
		if (ServeValue(it.value(), keys.sounds.count(it.key()) > 0, w)) {
			AddSound(out.sounds, it.value());
		}
	}
	if (!resolved.scopes.is_object() || !resolved.scopes.contains("events") ||
	    !resolved.scopes["events"].is_array()) {
		return out;
	}
	json scopes = json::object();
	scopes["events"] = resolved.scopes["events"];
	// A fork is served flat Defaults: its own code never asked for the layers, and handing
	// them over would let a stored override change what an unmodified fork renders.
	if (!w.IsForked()) {
		json overrides = json::object();
		for (auto it = w.overrides.begin(); it != w.overrides.end(); ++it) {
			if (it->is_object()) {
				overrides[it.key()] = ServeLayer(it.value(), keys, w, out.sounds);
			}
		}
		json variations = json::array();
		for (const json &v : w.variations) {
			if (!v.is_object()) {
				continue;
			}
			json copy = v;
			copy["settings"] = ServeLayer(v.value("settings", json::object()), keys, w, out.sounds);
			variations.push_back(std::move(copy));
		}
		scopes["overrides"] = std::move(overrides);
		scopes["variations"] = std::move(variations);
	}
	out.scopes = std::move(scopes);
	return out;
}

std::optional<std::string> LibraryFileFor(const std::string &id)
{
	std::lock_guard<std::mutex> lock(g_manifestMutex);
	if (!g_manifest) {
		g_manifest = ReadManifest();
		if (!g_manifest) {
			HostLog("[overlay] the sound library manifest at " + LibraryRoot() + "/" + kLibraryManifest +
				" did not read; library sounds are not served");
			return std::nullopt;
		}
	}
	const auto it = g_manifest->find(id);
	if (it == g_manifest->end()) {
		return std::nullopt;
	}
	return it->second;
}

} // namespace Overlay
