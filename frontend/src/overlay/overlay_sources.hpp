#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SOURCES_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SOURCES_HPP_

#include <string>

struct obs_source;
typedef struct obs_source obs_source_t;
struct obs_data;
typedef struct obs_data obs_data_t;
struct obs_data_array;
typedef struct obs_data_array obs_data_array_t;

namespace Overlay {

// The obs source type obs-browser registers for stream overlays, and the obs_data
// key such a source persists. The key holds an overlay id, never a URL: the loopback
// port can move between runs (the server scans five bands when its persisted port is
// taken), so a saved URL is a URL that eventually 404s.
inline constexpr char kOverlaySourceId[] = "braidcast_overlay";
inline constexpr char kOverlayIdKey[] = "overlay_id";

// The settings that decide whether an overlay's audio goes through OBS -- and so whether
// the mixer lists it and CEF runs audio capture plus its keepalive for the page at all.
//
// The first two belong to obs-browser (REROUTE_AUDIO_KEY, OVERLAY_REROUTE_MIGRATED_KEY), a
// separate repo that libobs only reaches through obs_data, so these strings ARE the
// contract; this is the frontend's one spelling of them. Two plugin behaviours every writer
// here has to respect: braidcast_overlay defaults reroute_audio to TRUE, and on its first
// update it flips a persisted false back to true unless the migrated marker is set -- so a
// false written without the marker does not survive.
inline constexpr char kRerouteAudioKey[] = "reroute_audio";
inline constexpr char kRerouteMigratedKey[] = "braidcast_reroute_migrated";
// Frontend-owned: who decided reroute_audio. Absent on a source saved before the frontend
// decided anything, which is what marks a source for the migration's opt-out reading (see
// SyncSavedReroute); "template" while it follows its overlay's template, on every load
// too; "user" once the user set it in the properties form, after which nothing automatic
// touches it again. Restore Defaults clears it with everything else.
inline constexpr char kRerouteOwnerKey[] = "braidcast_reroute_owner";
inline constexpr char kRerouteOwnerTemplate[] = "template";
inline constexpr char kRerouteOwnerUser[] = "user";

// Is `source` one of this fork's overlay widget sources? The single spelling of the
// type test: the viewport follow, the properties form and the refresh/count sweeps all
// key off it, and an id compared by hand at each site is one rename from drifting.
// Null-safe. Compares the UNVERSIONED id, so a future versioned variant still matches.
bool IsOverlaySource(obs_source_t *source);

// Will `source`'s page URL resolve RIGHT NOW? The precondition for updating an overlay
// source at all: braidcast_overlay re-resolves its URL from the url proc on EVERY update,
// so when the resolve yields nothing the url falls back to about:blank, compares unequal
// to the page currently loaded, and obs-browser answers with DestroyBrowser +
// create_browser -- a widget that was live on air goes blank. Three ways to yield
// nothing: no overlay bound, no server listening, or the bound widget deleted from the
// store; this reports all three, exactly as the proc decides them.
bool IsOverlayUrlResolvable(obs_source_t *source);

// Publish the overlay list + per-id URL lookup on the libobs GLOBAL proc handler, so
// obs-browser -- a separately loaded module sharing this process and this libobs --
// resolves an id without re-reading overlays.json (which would duplicate portable-mode
// path resolution and race the server's port re-persist). Must run after obs_startup
// and before module load. The handler is created by obs_startup and freed by
// obs_shutdown, and libobs exposes no proc removal, so the callbacks are stateless and
// there is nothing to undo in ObsBootstrap::Stop.
void RegisterProcs();

// Re-update every live overlay source so it re-resolves its URL. This is the path by
// which the sources bound to a DELETED overlay go blank (Bridge's overlays.delete
// calls it); resolution at load needs no help from it.
//
// It deliberately does NOT run after the server binds, and that rests on an ordering
// invariant in ObsBootstrap::Start: OverlayServer::Start() precedes
// LoadCuratedModules(), which is what registers braidcast_overlay. Until that module
// load, obs_source_create("braidcast_overlay", ...) returns null -- so no overlay
// source can exist before a port does, and none is ever created against a server that
// is not listening. Every other creation path (scene-collection switch, undo/redo,
// duplicate, import, the MCP create_source tool) is necessarily later still.
// If Start() is ever moved back below module load, a post-bind sweep has to come back
// with it. UI thread only.
//
// Each source's update also carries its template's audio route (FollowTemplateReroute),
// because both store mutations that can move it -- fork and return to stock -- end in this
// sweep, and folding it into the update the sweep already makes costs no second page
// reload. A delete leaves the route as it was: the id then names no widget.
void RefreshSources();

// RefreshSources for one source: re-resolve its URL, and follow its template's audio route
// unless the user owns that setting. No-op for anything but a braidcast_overlay. UI thread.
void RefreshSource(obs_source_t *source);

// What FollowTemplateReroute decided.
enum class RerouteDecision {
	Unidentified, // no widget has that id (or no id at all): left exactly as it was
	UserOwned,    // the user chose the value, so it stands
	Unchanged,    // now template-owned, already carrying the template's value
	Changed,      // now template-owned, and the value moved to the template's
};

// Decide reroute_audio for an overlay source bound to `overlayId`, reading its state from
// `current` and writing the decision into `out` -- the same object for a settings bag being
// built, a patch for a live source. The template's value is on for a widget that may play
// audio (Widget::MayPlayAudio) and off for a stock silent one.
//
// Writes nothing when the id names no widget: an overlay that cannot be identified may
// play sound, and the plugin default (on) is already the answer for it. Writes nothing but
// the owner when `current` has no owner yet and holds a deliberate opt-out -- an explicit
// false carrying the plugin's migrated marker, which obs-browser already treats as the
// user's choice. Every template-owned write sets that marker too, or the plugin would undo
// a false.
RerouteDecision FollowTemplateReroute(obs_data_t *current, const char *overlayId, obs_data_t *out);

// FollowTemplateReroute for a caller that already holds the widget, so it need not be looked
// up twice: `mayPlayAudio` is that widget's Widget::MayPlayAudio. Never Unidentified.
RerouteDecision ApplyTemplateReroute(obs_data_t *current, bool mayPlayAudio, obs_data_t *out);

// Bring every braidcast_overlay in a scene collection's saved "sources" array into step with
// its overlay's CURRENT template, BEFORE obs_load_sources sees it: FollowTemplateReroute over
// each one that is not user-owned. Rewriting the saved data rather than the loaded sources
// means the page is created with its final route -- no second update, no reload. It cannot
// keep a silent source out of the mixer outright: libobs starts every source audio-active
// until its first deferred update runs, so a mixer rebuild inside that window can still
// list it briefly, as it can any plain browser source.
//
// It runs on every load, not once, because widgets are global (overlays.json) while
// sources belong to a collection: a widget forked or returned to stock while another
// collection was active -- or before a crash that lost the next Save -- leaves its sources
// saved with the old template's route, and RefreshSources only ever reaches live ones. Safe
// to repeat: a user-owned value is never touched, an unknown overlay is left alone, and a
// template-owned value only moves when the template's answer did.
//
// The first pass over a source saved before owners existed is the migration. One ambiguity
// it resolves toward silence: a persisted TRUE on a silent built-in cannot be told apart
// from the default, from obs-browser's own forced flip, or from a user who ticked the box,
// so all of them go off. An explicit false with the plugin's marker is the one state that
// is provably the user's, and it becomes user-owned. After that pass every source whose
// overlay was identified carries an owner, so the opt-out reading never applies to it
// again: a template-owned false is the template's, not the user's.
void SyncSavedReroute(obs_data_array_t *sources);

// SyncSavedReroute for one saved-source record (obs_save_source's shape), for the paths that
// recreate a single source from saved data rather than a whole collection: undo's restore
// of a removed source or a duplicated scene's children. No-op for any other type.
void SyncSavedSource(obs_data_t *sourceData);

// obs_source_update(source, patch) with an overlay's audio-route consequence folded into the
// patch first, so the change lands in the one update the patch was going to cause anyway:
// a patch that rebinds overlay_id follows the new widget's template unless the user owns
// the setting; otherwise a patch that changes reroute_audio from its current value makes
// the user its owner. The rebind is checked first, so a patch carrying both -- the form's
// Cancel restoring its snapshot -- follows the template rather than claiming the setting.
// Called from the "source" property kind's update, so properties.set and Restore Defaults
// both pass through it; for Restore Defaults it changes nothing, because the patch there is
// the source's own settings already cleared, so it carries neither a rebind nor a toggle.
// Any other source is updated with the patch as given. UI thread.
void ApplySettingsPatch(obs_source_t *source, obs_data_t *patch);

// How many live braidcast_overlay sources are bound to `overlayId`. The delete
// confirmation quotes it, so removing a widget scenes are still using is something the
// user decides rather than discovers: a source whose overlay is gone renders a blank
// page, which looks the same as a widget that draws nothing yet. Counts what
// obs_enum_sources enumerates -- the same set RefreshOne sweeps. UI thread only.
int CountSourcesUsing(const std::string &overlayId);

} // namespace Overlay

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SOURCES_HPP_
