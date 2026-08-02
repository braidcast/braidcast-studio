#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_STORE_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_STORE_HPP_

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Overlay {

using json = nlohmann::json;

// The on-disk definition of a widget type: the markup that renders it and the schema
// of the settings it understands. Owned by the app, shared by every stock widget of
// that type, which is what lets a template fix reach widgets that already exist.
// schema entries are {key,type,label,default,...type-specific(options|min|max|step)} --
// verbatim json, so a new field type is a data change and never a C++ branch.
struct TypeTemplate {
	std::string html;
	std::string css;
	std::string js;
	json schema = json::array();
	bool ok = false; // false when the type has no template on disk
};

// Loads and caches the template for a type. The cache lives to process exit: the files
// are staged into the rundir by the build, so they cannot change under a running app.
const TypeTemplate &TypeTemplateFor(const std::string &type);

// One overlay widget. assets[] is stored as verbatim json objects. asset: {key,kind,file}.
struct Widget {
	std::string id;    // uuid (os_generate_uuid)
	std::string token; // 32 hex chars (BCryptGenRandom)
	std::string name;
	std::string type; // "alertbox" (v1)
	// Only the values the user actually changed, keyed by schema field key. A key that
	// is absent tracks the type template's default -- which is the whole point: a
	// template that gains a field has it appear on every existing stock widget.
	json settings = json::object();
	// null while the widget is stock, so the type's template renders it. Becomes
	// {html,css,js,fields} the moment the user opts into custom code, after which this
	// widget is forked and app template changes no longer reach it.
	json custom = nullptr;
	json assets = json::array();
	// Bumped by every accepted Update. A widget's html/css/js live in the SERVED
	// DOCUMENT, not in an overlay source's settings, so an edit changes nothing a
	// browser source can observe; the revision is what makes the resolved URL differ,
	// which is the one input a browser source acts on. Persisted, so a source that
	// resolved r=7 before a restart is not handed r=0 afterwards and left showing the
	// page it already has.
	int rev = 0;

	bool IsCustom() const { return custom.is_object(); }

	// The markup this widget actually serves: its own copy once forked, the type's
	// template while stock.
	std::string Html() const;
	std::string Css() const;
	std::string Js() const;

	// {key: value} for the served page: the type's schema defaults with this widget's
	// settings laid over them, or the fork's own field values once custom.
	json FieldValues() const;

	json ToJson() const;             // full definition
	json ToListJson(int port) const; // {id,name,type,token,url} for overlays.list
	// Accepts both the current shape and the pre-fork records that carried their own
	// html/css/js/fields; see the migration note on the definition.
	static Widget FromJson(const json &j);
};

// Full loopback URL for a widget, "http://127.0.0.1:<port>/w/<id>?t=<token>".
std::string WidgetUrl(const Widget &w, int port);

// The persisted widget registry (global overlays.json). Full impl in Group 2; a
// minimal stub in Group 1 so the server + self-test compile.
class OverlayStore {
public:
	OverlayStore() { Load(); }

	std::vector<Widget> List() const;                       // copy, mutex-guarded
	std::optional<Widget> Get(const std::string &id) const; // by id
	int Port() const;                                       // persisted chosen port (default 43000)
	void SetPort(int port);                                 // persist a newly-bound port

	Widget Create(const std::string &name, const std::string &type); // seed from default template
	// {name?,html?,css?,js?,fields?}. Bumps the widget's revision and reports the new
	// value through *newRev, so a caller can hand it back to the editor rather than
	// re-reading the widget just to learn it.
	bool Update(const std::string &id, const json &patch, int *newRev = nullptr);
	// Fork a stock widget onto its own copy of the type's template, or drop a fork and
	// return it to stock. Snapshotting here rather than in the editor keeps the copy
	// atomic and means the UI never has to hold a template just to hand it back.
	// Reverting discards the fork's code; the caller is expected to have confirmed.
	bool SetCustom(const std::string &id, bool enabled, int *newRev = nullptr);
	std::optional<Widget> Duplicate(const std::string &id); // new id+token, assets copied
	bool Delete(const std::string &id);                     // removes widget + overlays/<id> dir
	// Store a decoded asset file; returns its served relative path "assets/<file>" (or "" on failure).
	std::string AddAsset(const std::string &id, const std::string &key, const std::string &kind,
			     const std::vector<unsigned char> &bytes);
	// Drop one stored asset: its record and its file. `file` is the served basename as it
	// appears in the widget's assets[]; it is sanitized before it reaches the filesystem.
	bool RemoveAsset(const std::string &id, const std::string &file);

	static std::string FilePath();                       // MultistreamBasicPath("overlays.json")
	static std::string AssetsDir(const std::string &id); // .../basic/overlays/<id>/assets

	// Test-only: inject a widget into the in-memory set without persisting (self-test).
	void InjectForTest(const Widget &w);
	// Test-only: undo an InjectForTest so the shared singleton is left clean after a run.
	void RemoveForTest(const std::string &id);

private:
	void Load();
	void Save() const; // caller holds mutex_

	mutable std::mutex mutex_;
	std::vector<Widget> widgets_;
	int port_ = 43000;
};

OverlayStore &Store(); // function-local static singleton (lives to process exit)

// Forward-declare the server accessor (defined in overlay_server.cpp) so callers
// only include this header.
class OverlayServer;
OverlayServer &Server();

} // namespace Overlay

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_STORE_HPP_
