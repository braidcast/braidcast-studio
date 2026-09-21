#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/json_util.hpp"
#include "util/time_util.hpp"

// The store-agnostic half of a capped most-recently-used history: the ordering, the cap, the
// monotonic "used now" stamp and the tolerant stamp reader. Shared by every store that keeps
// such a list (StreamInfoPresetStore, PollTemplateStore) so the eviction rule cannot drift
// between them. A row type needs a `std::string id` and an `int64_t lastUsedAtMs`; the vector
// is kept most recently used first.
namespace MruRows {

// An epoch-ms field, falling back to `fallback` for a missing, non-numeric or non-positive
// value. Zero is refused along with garbage: a hand-edited row carrying one would be the
// next eviction victim regardless of how recently it was really used. The upper clamp is
// what keeps UsedNowMs()'s `front().lastUsedAtMs + 1` from overflowing on a document
// carrying INT64_MAX.
inline int64_t ReadTimestamp(const nlohmann::json &item, const char *key, int64_t fallback)
{
	constexpr int64_t kMaxStampMs = std::numeric_limits<int64_t>::max() / 2;
	const int64_t value = JsonUtil::NumLoose(item, key, fallback);
	return value > 0 ? std::min(value, kMaxStampMs) : fallback;
}

// The row holding `id`, or rows.end().
template<typename Row> typename std::vector<Row>::iterator FindById(std::vector<Row> &rows, const std::string &id)
{
	return std::find_if(rows.begin(), rows.end(), [&id](const Row &row) { return row.id == id; });
}

// The stamp a row takes when it is used: the wall clock, or one past the most recent stamp
// already held when the wall clock does not exceed it. Eviction reads this field (see
// Normalize), and TimeUtil::NowMs is system_clock -- so an NTP correction, a VM resume or a
// manual time change that moves the clock BACKWARD would otherwise stamp the row the user
// just applied below every other row and make it the next one dropped. The clamp keeps the
// field usable as the date the picker shows while making the order it drives monotonic. The
// trade it accepts: while the clock is behind, the shown "last used" time leads the real one,
// by at most how far back the clock went.
//
// Reads front() as the largest stamp held, so the caller must keep `rows` normalized: only
// erasing a row or changing a field other than the stamp may skip Normalize.
template<typename Row> int64_t UsedNowMs(const std::vector<Row> &rows)
{
	const int64_t now = TimeUtil::NowMs();
	if (rows.empty()) {
		return now;
	}
	return std::max(now, rows.front().lastUsedAtMs + 1);
}

// Restore the invariant: ordered by last use, most recent first, and never longer than `cap`.
template<typename Row> void Normalize(std::vector<Row> &rows, size_t cap)
{
	// Stable, so two rows stamped in the same millisecond keep a deterministic order.
	std::stable_sort(rows.begin(), rows.end(),
			 [](const Row &a, const Row &b) { return a.lastUsedAtMs > b.lastUsedAtMs; });
	// Eviction takes the tail, which is the row used longest ago -- never the oldest by
	// creation. A row made months back but used every broadcast has to survive a burst of
	// one-off experiments.
	if (rows.size() > cap) {
		rows.resize(cap);
	}
}

} // namespace MruRows
