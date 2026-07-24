#pragma once

namespace Braidcast {

// `git describe --tags --always --dirty` of the tree this binary was built from
// (e.g. "v0.4.1", "v0.4.0-3-gabc1234", "abc1234-dirty", or "unknown" when git is
// unavailable). Stamped at build time and logged at startup so every session log
// identifies its exact build -- no more inferring freshness from binary timestamps.
const char *BuildDescribe();

} // namespace Braidcast
