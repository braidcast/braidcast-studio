#ifndef OBS_MULTISTREAM_FRONTEND_FONTS_HPP_
#define OBS_MULTISTREAM_FRONTEND_FONTS_HPP_

#include <string>
#include <vector>

// The installed font families, as the UI's font fields offer them.
namespace Fonts {

// Every installed family name, sorted case-insensitively and deduplicated; empty when
// the system font collection could not be read.
//
// Built on the first call and held for the process. Walking the whole collection costs
// real time, and nothing in the app needs the list until a font field is opened, so boot
// must not pay for it. The flip side is that a font installed mid-session is not offered
// until the next launch -- a suggestion list, not an authority, and the field it feeds
// stays free text either way.
//
// Callable from any thread, and NOT from TID_UI: the bridge reaches it on the async lane
// precisely because the first call blocks for as long as the walk takes. Two callers
// racing the first call is safe -- the second blocks on the static's initialization and
// both see the same vector, which is then read-only for the rest of the process.
const std::vector<std::string> &ListFamilies();

} // namespace Fonts

#endif // OBS_MULTISTREAM_FRONTEND_FONTS_HPP_
