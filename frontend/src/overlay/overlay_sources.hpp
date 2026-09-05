#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SOURCES_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SOURCES_HPP_

#include <string>

struct obs_source;
typedef struct obs_source obs_source_t;

namespace Overlay {

// The obs source type obs-browser registers for stream overlays, and the obs_data
// key such a source persists. The key holds an overlay id, never a URL: the loopback
// port can move between runs (the server scans five bands when its persisted port is
// taken), so a saved URL is a URL that eventually 404s.
inline constexpr char kOverlaySourceId[] = "braidcast_overlay";
inline constexpr char kOverlayIdKey[] = "overlay_id";

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
void RefreshSources();

// How many live braidcast_overlay sources are bound to `overlayId`. The delete
// confirmation quotes it, so removing a widget scenes are still using is something the
// user decides rather than discovers: a source whose overlay is gone renders a blank
// page, which looks the same as a widget that draws nothing yet. Counts what
// obs_enum_sources enumerates -- the same set RefreshOne sweeps. UI thread only.
int CountSourcesUsing(const std::string &overlayId);

} // namespace Overlay

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SOURCES_HPP_
