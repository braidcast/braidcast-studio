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
	std::string token; // 32 hex chars (BCryptGenRandom)
	std::string name;
	std::string type; // "alertbox" (v1)
	std::string html;
	std::string css;
	std::string js;
	json fields = json::array();
	json assets = json::array();

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

	Widget Create(const std::string &name, const std::string &type); // seed from default template
	bool Update(const std::string &id, const json &patch);           // {name?,html?,css?,js?,fields?}
	std::optional<Widget> Duplicate(const std::string &id);          // new id+token
	bool Delete(const std::string &id);                              // removes widget + overlays/<id> dir
	// Store a decoded asset file; returns its served relative path "assets/<file>" (or "" on failure).
	std::string AddAsset(const std::string &id, const std::string &key, const std::string &kind,
			     const std::vector<unsigned char> &bytes);

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
