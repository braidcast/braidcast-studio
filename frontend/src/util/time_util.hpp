#ifndef OBS_MULTISTREAM_FRONTEND_TIME_UTIL_HPP_
#define OBS_MULTISTREAM_FRONTEND_TIME_UTIL_HPP_

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>

// Time helpers shared by the chat/event integrations. Kept in ONE place so the
// return type can't drift per translation unit (the drift these replace: a `double`
// / `long long` NowMs / Rfc3339ToEpochMs alongside the int64_t copies).
namespace TimeUtil {

// One day in milliseconds, the unit NowMs() counts in.
inline constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;

// Current wall-clock time in epoch milliseconds.
inline int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

// Parse an RFC3339 / ISO-8601 instant ("2024-01-02T03:04:05.678Z", or with an offset:
// "2024-01-02T08:34:05+05:30", "+0530") into epoch milliseconds, or nothing when it does not
// parse. An offset is applied, so every form of one instant reads the same; a time with no
// zone at all reads as UTC. MSVC UTC mktime (_mkgmtime).
inline std::optional<int64_t> TryRfc3339ToEpochMs(const std::string &iso)
{
	int y = 0, mon = 0, d = 0, h = 0, mi = 0, s = 0, used = 0;
	if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d%n", &y, &mon, &d, &h, &mi, &s, &used) != 6) {
		return std::nullopt;
	}
	std::tm tm{};
	tm.tm_year = y - 1900;
	tm.tm_mon = mon - 1;
	tm.tm_mday = d;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = s;
	const std::time_t epoch = _mkgmtime(&tm);
	if (epoch == static_cast<std::time_t>(-1)) {
		return std::nullopt;
	}
	auto digit = [&iso](size_t i) {
		return i < iso.size() && std::isdigit(static_cast<unsigned char>(iso[i]));
	};
	size_t i = static_cast<size_t>(used);
	int64_t millis = 0;
	// Optional fractional seconds: the first 3 digits count, the rest are skipped.
	if (i < iso.size() && iso[i] == '.') {
		int place = 100;
		for (++i; digit(i); ++i) {
			millis += place * (iso[i] - '0');
			place /= 10;
		}
	}
	int64_t offsetMin = 0;
	if (i < iso.size() && (iso[i] == '+' || iso[i] == '-')) {
		const int sign = iso[i] == '-' ? -1 : 1;
		// hh, then mm with or without a ':' between them.
		if (!digit(i + 1) || !digit(i + 2)) {
			return std::nullopt;
		}
		const int oh = (iso[i + 1] - '0') * 10 + (iso[i + 2] - '0');
		size_t m = i + 3;
		if (m < iso.size() && iso[m] == ':') {
			++m;
		}
		int om = 0;
		if (digit(m) && digit(m + 1)) {
			om = (iso[m] - '0') * 10 + (iso[m + 1] - '0');
		}
		if (oh > 23 || om > 59) {
			return std::nullopt;
		}
		offsetMin = sign * (oh * 60 + om);
	}
	// The clock reads local time at that offset, so UTC is that minus the offset.
	return static_cast<int64_t>(epoch) * 1000 + millis - offsetMin * 60 * 1000;
}

// TryRfc3339ToEpochMs, falling back to the current wall clock on a parse failure so an
// event never carries a zero/garbage timestamp.
inline int64_t Rfc3339ToEpochMs(const std::string &iso)
{
	return TryRfc3339ToEpochMs(iso).value_or(NowMs());
}

// Current UTC time as an RFC3339 instant ("2024-01-02T03:04:05Z"), the shape
// YouTube's scheduledStartTime wants and the one Phase 3's scheduled start times
// will need.
inline std::string NowIso8601Utc()
{
	const std::time_t now = std::time(nullptr);
	std::tm tm{};
	gmtime_s(&tm, &now);
	char buf[32];
	std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
	return std::string(buf);
}

// Current local time as a file-name stamp ("2024-01-02_03-04-05"): sorts
// chronologically and holds no character a Windows file name refuses.
inline std::string LocalFileStamp()
{
	const std::time_t now = std::time(nullptr);
	std::tm tm{};
	localtime_s(&tm, &now);
	char buf[32];
	std::strftime(buf, sizeof buf, "%Y-%m-%d_%H-%M-%S", &tm);
	return std::string(buf);
}

} // namespace TimeUtil

#endif // OBS_MULTISTREAM_FRONTEND_TIME_UTIL_HPP_
