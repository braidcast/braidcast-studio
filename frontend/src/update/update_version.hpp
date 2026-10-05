#pragma once

#include <array>
#include <optional>
#include <string>

// Version strings as the update check meets them: a release tag ("v0.10.0") and the
// running build's OBS_VERSION, which git describe can suffix ("0.9.0-12-gabc1234",
// "0.10.0-rc1"). Pure, so the comparison is tested without a network or a build.
namespace Update {

// The leading major.minor.patch of `s`, a "v" prefix allowed and a missing minor or patch
// read as 0; nothing when it does not start with a number.
std::optional<std::array<int, 3>> ParseVersion(const std::string &s);

// Whether `candidate` is a later release than `running`. False when either does not parse,
// so a build whose version cannot be read is never told it is out of date.
bool IsNewer(const std::string &candidate, const std::string &running);

} // namespace Update
