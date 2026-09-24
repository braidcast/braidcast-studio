#include "overlay_sources.hpp"

#include "overlay_server.hpp"
#include "overlay_store.hpp"

#include "../log.hpp"

#include <obs.h>
#include <obs.hpp>

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
// The three conditions under which the url proc yields a URL. Factored out so the proc
// and the resize guard (IsOverlayUrlResolvable) can never disagree about what
// "resolvable" means -- a resize that writes while the resolve would fail blanks a live
// widget, so the two have to be one predicate rather than two that look alike.
bool UrlResolvable(const char *id)
{
	return id != nullptr && *id != '\0' && Server().IsListening() && Store().Get(id).has_value();
}

void ProcUrl(void * /*data*/, calldata_t *cd)
{
	const char *id = calldata_string(cd, "id");
	if (!UrlResolvable(id)) {
		return;
	}
	const std::optional<Widget> w = Store().Get(id);
	if (!w) {
		return;
	}
	// The revision rides the query string because a browser source reloads on a changed
	// URL and on nothing else: a widget's html/css/js are assembled into the served
	// document rather than stored in the source, so an edit leaves every observable
	// setting identical. It stays out of WidgetUrl -- the URL a user copies by hand into
	// a plain Browser Source would otherwise pin a revision that goes stale.
	calldata_set_string(cd, "url", (WidgetUrl(*w, Store().Port()) + "&r=" + std::to_string(w->rev)).c_str());
}

bool RefreshOne(void * /*param*/, obs_source_t *source)
{
	RefreshSource(source);
	return true;
}

// reroute_audio as obs-browser will apply it from these settings. A value never set is the
// type's default (on); an explicit false without the migrated marker is one the plugin
// flips back on at its first update.
bool EffectiveReroute(obs_data_t *settings)
{
	if (!obs_data_has_user_value(settings, kRerouteAudioKey) || obs_data_get_bool(settings, kRerouteAudioKey)) {
		return true;
	}
	return !obs_data_get_bool(settings, kRerouteMigratedKey);
}

// The one persisted state that is provably the user's: obs-browser's migration forced every
// unmarked false on and then marked it, so a false that carries the marker was set after.
bool IsDeliberateOptOut(obs_data_t *settings)
{
	return obs_data_has_user_value(settings, kRerouteAudioKey) && !obs_data_get_bool(settings, kRerouteAudioKey) &&
	       obs_data_get_bool(settings, kRerouteMigratedKey);
}

bool IsUserOwned(obs_data_t *settings)
{
	return strcmp(obs_data_get_string(settings, kRerouteOwnerKey), kRerouteOwnerUser) == 0;
}

const char *DescribeDecision(RerouteDecision d)
{
	switch (d) {
	case RerouteDecision::Unidentified:
		return "overlay unknown, left alone";
	case RerouteDecision::UserOwned:
		return "user-owned, left alone";
	case RerouteDecision::Unchanged:
		return "follows template, unchanged";
	case RerouteDecision::Changed:
		return "follows template, changed";
	}
	return "?";
}

// ApplySettingsPatch's first half; see there.
void PrepareSettingsPatch(obs_source_t *source, obs_data_t *patch)
{
	if (!IsOverlaySource(source) || patch == nullptr) {
		return;
	}
	OBSDataAutoRelease current = obs_source_get_settings(source);
	const char *name = obs_source_get_name(source);
	// The rebind is decided first because the properties form's Cancel sends its whole
	// open-time snapshot back: overlay_id AND reroute_audio. Read as a toggle, the restored
	// reroute value would hand the setting to the user; read as a rebind, it follows the
	// restored overlay's template, which is where it came from.
	const char *nextId = obs_data_get_string(patch, kOverlayIdKey);
	if (obs_data_has_user_value(patch, kOverlayIdKey) &&
	    strcmp(nextId, obs_data_get_string(current, kOverlayIdKey)) != 0) {
		const RerouteDecision d = FollowTemplateReroute(current, nextId, patch);
		if (d == RerouteDecision::Unchanged || d == RerouteDecision::Changed) {
			DBG(LogCat::Overlay, "'%s' rebound to overlay '%s': audio route %s, reroute=%s", name, nextId,
			    DescribeDecision(d), obs_data_get_bool(patch, kRerouteAudioKey) ? "true" : "false");
			return;
		}
		DBG(LogCat::Overlay, "'%s' rebound to overlay '%s': audio route %s", name, nextId, DescribeDecision(d));
	}
	if (obs_data_has_user_value(patch, kRerouteAudioKey) &&
	    obs_data_get_bool(patch, kRerouteAudioKey) != EffectiveReroute(current)) {
		obs_data_set_bool(patch, kRerouteMigratedKey, true);
		obs_data_set_string(patch, kRerouteOwnerKey, kRerouteOwnerUser);
		DBG(LogCat::Overlay, "'%s' audio route set by the user: reroute=%s", name,
		    obs_data_get_bool(patch, kRerouteAudioKey) ? "true" : "false");
	}
}

struct UsageScan {
	const std::string *overlayId;
	int count;
};

bool CountOne(void *param, obs_source_t *source)
{
	if (!IsOverlaySource(source)) {
		return true;
	}
	UsageScan *scan = static_cast<UsageScan *>(param);
	obs_data_t *settings = obs_source_get_settings(source);
	if (settings != nullptr) {
		const char *bound = obs_data_get_string(settings, kOverlayIdKey);
		if (bound != nullptr && *scan->overlayId == bound) {
			++scan->count;
		}
		obs_data_release(settings);
	}
	return true;
}

} // namespace

bool IsOverlaySource(obs_source_t *source)
{
	if (source == nullptr) {
		return false;
	}
	const char *id = obs_source_get_unversioned_id(source);
	return id != nullptr && strcmp(id, kOverlaySourceId) == 0;
}

bool IsOverlayUrlResolvable(obs_source_t *source)
{
	if (!IsOverlaySource(source)) {
		return false;
	}
	obs_data_t *settings = obs_source_get_settings(source); // addref'd
	if (settings == nullptr) {
		return false;
	}
	const bool resolvable = UrlResolvable(obs_data_get_string(settings, kOverlayIdKey));
	obs_data_release(settings);
	return resolvable;
}

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

void RefreshSource(obs_source_t *source)
{
	if (!IsOverlaySource(source)) {
		return;
	}
	OBSDataAutoRelease current = obs_source_get_settings(source);
	OBSDataAutoRelease patch = obs_data_create();
	const RerouteDecision d = FollowTemplateReroute(current, obs_data_get_string(current, kOverlayIdKey), patch);
	if (d == RerouteDecision::Changed) {
		HostLog(std::string("[overlay] '") + obs_source_get_name(source) + "' audio route now reroute=" +
			(obs_data_get_bool(patch, kRerouteAudioKey) ? "true" : "false") + " (its template changed)");
	}
	obs_source_update(source, patch);
}

RerouteDecision ApplyTemplateReroute(obs_data_t *current, bool mayPlayAudio, obs_data_t *out)
{
	if (IsUserOwned(current)) {
		return RerouteDecision::UserOwned;
	}
	if (obs_data_get_string(current, kRerouteOwnerKey)[0] == '\0' && IsDeliberateOptOut(current)) {
		obs_data_set_string(out, kRerouteOwnerKey, kRerouteOwnerUser);
		return RerouteDecision::UserOwned;
	}
	const bool was = EffectiveReroute(current);
	obs_data_set_bool(out, kRerouteAudioKey, mayPlayAudio);
	obs_data_set_bool(out, kRerouteMigratedKey, true);
	obs_data_set_string(out, kRerouteOwnerKey, kRerouteOwnerTemplate);
	return mayPlayAudio == was ? RerouteDecision::Unchanged : RerouteDecision::Changed;
}

RerouteDecision FollowTemplateReroute(obs_data_t *current, const char *overlayId, obs_data_t *out)
{
	const std::optional<bool> mayPlayAudio =
		(overlayId != nullptr && *overlayId != '\0') ? Store().MayPlayAudio(overlayId) : std::nullopt;
	if (!mayPlayAudio) {
		return RerouteDecision::Unidentified;
	}
	return ApplyTemplateReroute(current, *mayPlayAudio, out);
}

void SyncSavedSource(obs_data_t *sourceData)
{
	// "id" is the unversioned id obs_save_source writes, the same one IsOverlaySource
	// compares, so a versioned variant of the type is synced too.
	if (sourceData == nullptr || strcmp(obs_data_get_string(sourceData, "id"), kOverlaySourceId) != 0) {
		return;
	}
	OBSDataAutoRelease settings = obs_data_get_obj(sourceData, "settings");
	if (!settings) {
		return;
	}
	const char *name = obs_data_get_string(sourceData, "name");
	const bool hadOwner = obs_data_has_user_value(settings, kRerouteOwnerKey);
	const RerouteDecision d =
		FollowTemplateReroute(settings, obs_data_get_string(settings, kOverlayIdKey), settings);
	// The always-on log gets the first decision a source ever receives and every value that
	// moves; a template-owned source already in step, or one whose overlay is unknown (asked
	// again on every load), would otherwise repeat itself on each one.
	if (d == RerouteDecision::Changed || (!hadOwner && d != RerouteDecision::Unidentified)) {
		HostLog(std::string("[overlay] audio route of '") + name + "' on load: " + DescribeDecision(d) +
			", reroute=" + (EffectiveReroute(settings) ? "true" : "false"));
	} else {
		DBG(LogCat::Overlay, "audio route of '%s' on load: %s", name, DescribeDecision(d));
	}
}

void SyncSavedReroute(obs_data_array_t *sources)
{
	const size_t count = obs_data_array_count(sources);
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease item = obs_data_array_item(sources, i);
		SyncSavedSource(item);
	}
}

void ApplySettingsPatch(obs_source_t *source, obs_data_t *patch)
{
	PrepareSettingsPatch(source, patch);
	obs_source_update(source, patch);
}

int CountSourcesUsing(const std::string &overlayId)
{
	UsageScan scan{&overlayId, 0};
	obs_enum_sources(&CountOne, &scan);
	return scan.count;
}

} // namespace Overlay
