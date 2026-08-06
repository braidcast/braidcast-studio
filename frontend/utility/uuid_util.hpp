#ifndef OBS_MULTISTREAM_FRONTEND_UUID_UTIL_HPP_
#define OBS_MULTISTREAM_FRONTEND_UUID_UTIL_HPP_

#include <string>

// Identity values -- canvas, stream-profile, output-binding, scene-collection,
// overlay-widget and recorded-session uuids. These name things, they do not
// protect them, so they come from the platform UUID generator and deliberately
// NOT from RandomUtil, which exists for secrets and should stay auditable as
// such.
//
// Lives beside the data-model types rather than in src/util/ because those types
// mint their own ids, and utility/ is the layer src/ builds on -- it cannot reach
// upward for this.
namespace UuidUtil {

// A fresh uuid string. Empty if the platform generator fails.
std::string New();

} // namespace UuidUtil

#endif // OBS_MULTISTREAM_FRONTEND_UUID_UTIL_HPP_
