#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SCOPES_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SCOPES_HPP_

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace Overlay {

using json = nlohmann::json;

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

} // namespace Overlay

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SCOPES_HPP_
