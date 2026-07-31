#include "overlay_sources.hpp"

#include "overlay_server.hpp"
#include "overlay_store.hpp"

#include "../log.hpp"

#include <obs.h>

#include <nlohmann/json.hpp>

#include <cstring>
#include <optional>
#include <string>

namespace Overlay {

namespace {

using json = nlohmann::json;

// The two declarations below are the cross-module contract: obs-browser calls these
// proc names and reads these out-parameter names. It lives in another repository, so
// a rename here is only half a rename.
constexpr char kListDecl[] = "void braidcast_overlay_list(out string overlays, out bool listening)";
constexpr char kUrlDecl[] = "void braidcast_overlay_url(in string id, out string url)";

// [{id,name}, ...] for the properties dropdown, plus whether the server that would
// serve them is actually up. Deliberately without the token or the URL: the picker only
// needs to name overlays, and a caller that wants a URL asks for one id at a time
// through the url proc.
//
// `listening` exists because the list alone is misleading when the bind failed: every
// widget still enumerates, so the picker would look healthy while every overlay renders
// blank. The picker greys itself out instead.
void ProcList(void * /*data*/, calldata_t *cd)
{
	json arr = json::array();
	for (const Widget &w : Store().List()) {
		arr.push_back(json{{"id", w.id}, {"name", w.name}});
	}
	calldata_set_string(cd, "overlays", arr.dump().c_str());
	calldata_set_bool(cd, "listening", Server().IsListening());
}

// One overlay's CURRENT loopback URL, or no out-parameter at all when there is nothing
// servable: an unknown id (the overlay was deleted), or a server that never bound (all
// 250 ports across the five scan bands taken -- a failure with no retry, which is what
// `listening` above surfaces to the picker).
//
// A source cannot reach here before the server has had its chance to bind: Start() runs
// ahead of the module load that registers the type, so resolution at load always sees
// the final port. Leaving the parameter absent rather than composing one from the
// persisted port keeps the caller from pointing CEF at a port nothing is listening on.
void ProcUrl(void * /*data*/, calldata_t *cd)
{
	const char *id = calldata_string(cd, "id");
	if (id == nullptr || *id == '\0' || !Server().IsListening()) {
		return;
	}
	const std::optional<Widget> w = Store().Get(id);
	if (!w) {
		return;
	}
	calldata_set_string(cd, "url", WidgetUrl(*w, Store().Port()).c_str());
}

bool RefreshOne(void * /*param*/, obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	if (id != nullptr && strcmp(id, kOverlaySourceId) == 0) {
		obs_source_update(source, nullptr);
	}
	return true;
}

} // namespace

void RegisterProcs()
{
	proc_handler_t *ph = obs_get_proc_handler();
	if (ph == nullptr) {
		HostLog("[overlay] no global proc handler; overlay sources cannot resolve their URL");
		return;
	}
	proc_handler_add(ph, kListDecl, ProcList, nullptr);
	proc_handler_add(ph, kUrlDecl, ProcUrl, nullptr);
	DBG(LogCat::Overlay, "overlay source procs registered");
}

void RefreshSources()
{
	obs_enum_sources(&RefreshOne, nullptr);
}

} // namespace Overlay
