#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SCOPES_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SCOPES_HPP_

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "overlay_store.hpp"

namespace Overlay {

using json = nlohmann::json;

// The server's route for the library, and the prefix every library URL it hands a page
// starts with.
inline constexpr char kLibraryRoutePrefix[] = "/lib/";

// The bundled sound library (web/public/overlay/library/), served at /lib/<path> with no
// token: public CC0 files from the app's own bundle on a loopback server.
std::string LibraryRoot();

// Whether `rel` may be served from the library: one or two segments of [A-Za-z0-9._-], none
// empty or starting with a dot, and resolving inside LibraryRoot(). Rejects "..", absolute
// paths, drive letters, backslashes and anything percent-encoded, since none of the
// library's own names need them.
bool IsSafeLibraryPath(const std::string &rel);

// The library file a sound id names, relative to LibraryRoot() ("sounds/chime-01.ogg"), from
// the shipped manifest (sounds.json). Nullopt for an id the manifest does not list, and for
// every id while the manifest does not read -- a failed read is retried on the next ask.
std::optional<std::string> LibraryFileFor(const std::string &id);

// What a widget's page is handed, built from its stored values. Stored values stay portable
// ("assets/<file>", "library:<id>"); only this copy is rewritten to URLs this server serves.
struct ServedData {
	// Every key the schema declares, at the widget's Defaults override or the schema default.
	json fields = json::object();
	// {events, overrides, variations} for a stock widget whose type declares scopes, {events}
	// for a fork of one (its code still reads each event's built-in message), null otherwise.
	// The scope layers keep only keys the schema lets an alert scope set.
	json scopes = json(nullptr);
	// Every sound any scope could play, de-duplicated, so the runtime decodes each once at
	// load -- including one that only a variation names.
	json sounds = json::array();
};
ServedData BuildServedData(const Widget &w, const ResolvedWidget &resolved);

} // namespace Overlay

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SCOPES_HPP_
