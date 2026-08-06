#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_STORE_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_STORE_HPP_

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Overlay {

using json = nlohmann::json;

// One overlay widget. fields[] / assets[] are stored as verbatim json objects so a
// new field type is a data change, never a C++ branch. field: {key,type,label,default,value,
// ...type-specific(options|min|max|step)}. asset: {key,kind,file}.
struct Widget {
	std::string id;    // uuid (os_generate_uuid)
	std::string token; // 32 hex chars; empty only when the RNG failed, and the server then serves nothing
	std::string name;
	std::string type; // "alertbox" (v1)
	std::string html;
	std::string css;
	std::string js;
	json fields = json::array();
	json assets = json::array();
	// Bumped by every accepted Update. A widget's html/css/js live in the SERVED
	// DOCUMENT, not in an overlay source's settings, so an edit changes nothing a
	// browser source can observe; the revision is what makes the resolved URL differ,
	// which is the one input a browser source acts on. Persisted, so a source that
	// resolved r=7 before a restart is not handed r=0 afterwards and left showing the
	// page it already has.
	int rev = 0;

	json ToJson() const;             // full definition
	json ToListJson(int port) const; // {id,name,type,token,url} for overlays.list
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

	// Seed a new widget from the default template for `type`. Nullopt when its access
	// token cannot be minted: a widget without one is unreachable through the server,
	// so registering it would only produce an overlay that 403s with no way to tell why.
	std::optional<Widget> Create(const std::string &name, const std::string &type);
	// {name?,html?,css?,js?,fields?}. Bumps the widget's revision and reports the new
	// value through *newRev, so a caller can hand it back to the editor rather than
	// re-reading the widget just to learn it.
	bool Update(const std::string &id, const json &patch, int *newRev = nullptr);
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
	void Save() const; // caller holds mutex_, or is Load() during construction

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
