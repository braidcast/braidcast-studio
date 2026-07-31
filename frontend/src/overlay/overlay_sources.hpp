#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SOURCES_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SOURCES_HPP_

namespace Overlay {

// The obs source type obs-browser registers for stream overlays, and the obs_data
// key such a source persists. The key holds an overlay id, never a URL: the loopback
// port can move between runs (the server scans five bands when its persisted port is
// taken), so a saved URL is a URL that eventually 404s.
inline constexpr char kOverlaySourceId[] = "braidcast_overlay";
inline constexpr char kOverlayIdKey[] = "overlay_id";

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

} // namespace Overlay

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SOURCES_HPP_
