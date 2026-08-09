#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_STORE_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_STORE_HPP_

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "overlay_template.hpp"

namespace Overlay {

using json = nlohmann::json;

// A widget's own copy of its code, present only once the user has explicitly forked it
// off the shipped template. fields[] is then that widget's whole schema, in the same
// shape a type's fields.json holds: {key,type,label,default,...}, values excluded.
struct CustomCode {
	std::string html;
	std::string css;
	std::string js;
	json fields = json::array();

	json ToJson() const;
	static CustomCode FromJson(const json &j);
};

// One overlay widget. settings/assets are stored as verbatim json objects so a new field
// type is a data change, never a C++ branch. asset: {key,kind,file}.
struct Widget {
	std::string id;    // uuid (os_generate_uuid)
	std::string token; // 32 hex chars; empty only when the RNG failed, and the server then serves nothing
	std::string name;
	std::string type; // one of the shipped widget types, e.g. "alertbox"
	// OVERRIDES ONLY, {schemaKey: value}. A key absent here tracks its schema's default
	// rather than holding a copy of it, which is what lets a field added to a shipped
	// fields.json show up on widgets that already exist.
	json settings = json::object();
	// Absent until the user forks. While it is absent the server serves
	// default-<type>/template.* and reads the schema from that type's fields.json, so an
	// improved template reaches this widget with no action from anyone.
	std::optional<CustomCode> custom;
	json assets = json::array();
	// Bumped by every accepted mutation. A widget's html/css/js live in the SERVED
	// DOCUMENT, not in an overlay source's settings, so an edit changes nothing a
	// browser source can observe; the revision is what makes the resolved URL differ,
	// which is the one input a browser source acts on. Persisted, so a source that
	// resolved r=7 before a restart is not handed r=0 afterwards and left showing the
	// page it already has.
	int rev = 0;

	bool IsForked() const { return custom.has_value(); }

	json ToJson() const;             // full definition
	json ToListJson(int port) const; // {id,name,type,token,url} for overlays.list
	static Widget FromJson(const json &j);
};

// One widget's schema and served code, taken together: its own once forked, the type's
// shipped template while stock. Resolved in a single pass rather than one call per part,
// so a stock widget's schema and its markup can never come from either side of a template
// re-read.
struct ResolvedWidget {
	json schema = json::array();
	std::string html;
	std::string css;
	std::string js;
};
ResolvedWidget Resolve(const Widget &w);

// Why a store mutation was refused; Ok is the only success. Shared by the three mutations
// that can be refused for more than one reason, so the bridge phrases each cause once.
enum class MutateResult {
	Ok,
	NoSuchWidget,  // the id names nothing (including "deleted while we were off the lock")
	NotForked,     // code or schema was submitted for a widget still on the shipped template
	AlreadyForked, // a fork was asked for on a widget that already owns its code
	AlreadyStock,  // a return to stock was asked for on a widget that has no code of its own
	NoTemplate,    // the type's template did not resolve, so there is nothing to fork from or return to
	// The write failed, so the widget was put back exactly as it was and nothing changed.
	// Reported rather than swallowed: a widget the editor was told it had forked, which is
	// stock again after a relaunch, costs the user every edit they made in between.
	NotPersisted,
};

// One clause naming why, for a bridge error message.
const char *DescribeMutateResult(MutateResult r);

// Full loopback URL for a widget, "http://127.0.0.1:<port>/w/<id>?t=<token>".
std::string WidgetUrl(const Widget &w, int port);

// The persisted widget registry (global overlays.json).
class OverlayStore {
public:
	OverlayStore() { Load(); }

	std::vector<Widget> List() const;                       // copy, mutex-guarded
	std::optional<Widget> Get(const std::string &id) const; // by id
	int Port() const;                                       // persisted chosen port (default 43000)
	void SetPort(int port);                                 // persist a newly-bound port

	// Register a new widget of `type`. It starts stock -- no code and no values are
	// copied out of the template -- so it serves whatever that type ships today and
	// keeps serving whatever it ships after the next update. Nullopt when its access
	// token cannot be minted: a widget without one is unreachable through the server,
	// so registering it would only produce an overlay that 403s with no way to tell why.
	std::optional<Widget> Create(const std::string &name, const std::string &type);
	// All three mutations below are atomic: the widget is snapshotted before it is
	// touched and put back verbatim -- revision included -- if the save does not land, so
	// a refusal always leaves the store exactly as the caller found it and never hands
	// out a revision that will not reach disk.
	//
	// "the type's template did not resolve", used below, means TemplateFor answered
	// anything but Ok: one of template.html/css/js/fields.json did not read (an unknown
	// type, an install missing a file, a file locked for a moment), or all four read and
	// fields.json did not parse as a field list (a truncated or hand-edited one).
	//
	// {name?,settings?,html?,css?,js?,fields?}. `settings` REPLACES the override set
	// wholesale -- the editor sends all of it, so a key it omits is a value returned to
	// its schema default rather than one left alone. html/css/js/fields are refused
	// (NotForked, widget untouched) unless the widget is forked, so saving a form can
	// never quietly detach a widget from its shipped template. Bumps the revision and
	// reports the new value through *newRev, so a caller can hand it back to the editor
	// rather than re-reading the widget just to learn it.
	MutateResult Update(const std::string &id, const json &patch, int *newRev = nullptr);
	// Take a private copy of the type's shipped template so the user can edit it: html,
	// css and js verbatim, and custom.fields seeded from that type's schema, so they
	// start from what they were already looking at. Values stay in settings and are
	// untouched. Refused when the widget is already forked, and when the type's template
	// did not resolve: a fork copies what it is handed and stores it, so a file that
	// happened not to read installs that gap permanently, and the only way back out of it
	// discards the user's code.
	// *custom, when given, receives the snapshot that was installed.
	MutateResult Fork(const std::string &id, int *newRev = nullptr, CustomCode *custom = nullptr);
	// Return the widget to the shipped template: drop `custom`, so the server serves
	// default-<type>/template.* again and that type's fields.json is the schema again.
	// `settings` SURVIVE -- configured values are not code, and the widget's own type
	// still declares the keys they are stored under, so returning to stock costs nothing
	// but the code edits it was asked to discard. Refused, widget untouched, when the
	// widget is already stock, and when that type's template did not resolve: pointing a
	// widget at a document that cannot be served leaves it blank with its own code
	// already gone.
	MutateResult ReturnToStock(const std::string &id, int *newRev = nullptr);
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
	// Caller holds mutex_, or is Load() during construction. False when the document did
	// not reach disk, so a mutator can roll its change back rather than report a success
	// the log quietly contradicts. Also false, always, while upgradeDeferred_ is set.
	bool Save() const;
	// Keep a copy of the pre-upgrade document beside `path` before the first migrated
	// save overwrites it. `asRead` is what Load() parsed, used only when the file itself
	// can no longer be read.
	void WritePreMigrationBackup(const std::string &path, const json &asRead) const;

	mutable std::mutex mutex_;
	std::vector<Widget> widgets_;
	int port_ = 43000;
	// Set when Load() declined to upgrade a v1 document because a type's template read
	// back incomplete, and never cleared: the decision is retried from scratch on the next
	// start. While it is set the store is READ-ONLY -- see Save() -- because the widgets in
	// memory are already converted and any save writes the whole document, so a single
	// SetPort at server start would commit the very verdict the deferral is refusing to
	// commit. Update, Fork and ReturnToStock surface that as NotPersisted; the other five
	// mutations do not check it and are lost at the next start, which is why Load() warns
	// once and says so. Save() carries the full account.
	bool upgradeDeferred_ = false;
};

OverlayStore &Store(); // function-local static singleton (lives to process exit)

// Forward-declare the server accessor (defined in overlay_server.cpp) so callers
// only include this header.
class OverlayServer;
OverlayServer &Server();

} // namespace Overlay

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_STORE_HPP_
