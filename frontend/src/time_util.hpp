#ifndef OBS_MULTISTREAM_FRONTEND_TIME_UTIL_HPP_
#define OBS_MULTISTREAM_FRONTEND_TIME_UTIL_HPP_

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

// Time helpers shared by the chat/event integrations. Kept in ONE place so the
// return type can't drift per translation unit (the drift these replace: a `double`
// / `long long` NowMs / Rfc3339ToEpochMs alongside the int64_t copies).
namespace TimeUtil {

// Current wall-clock time in epoch milliseconds.
inline int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
		.count();
}

// Parse an RFC3339 / ISO-8601 UTC instant ("2024-01-02T03:04:05.678Z") into epoch
// milliseconds; falls back to the current wall clock on a parse failure so an event
// never carries a zero/garbage timestamp. MSVC UTC mktime (_mkgmtime).
inline int64_t Rfc3339ToEpochMs(const std::string &iso)
{
	int y = 0, mon = 0, d = 0, h = 0, mi = 0, s = 0;
	if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mon, &d, &h, &mi, &s) == 6) {
		std::tm tm{};
		tm.tm_year = y - 1900;
		tm.tm_mon = mon - 1;
		tm.tm_mday = d;
		tm.tm_hour = h;
		tm.tm_min = mi;
		tm.tm_sec = s;
		const std::time_t epoch = _mkgmtime(&tm);
		if (epoch != static_cast<std::time_t>(-1)) {
			int64_t millis = 0;
			// Optional fractional seconds after a '.': capture up to 3 digits.
			const auto dot = iso.find('.');
			if (dot != std::string::npos) {
				std::string frac;
				for (size_t i = dot + 1;
				     i < iso.size() && std::isdigit(static_cast<unsigned char>(iso[i])) && frac.size() < 3;
				     ++i) {
					frac.push_back(iso[i]);
				}
				while (frac.size() < 3) {
					frac.push_back('0');
				}
				millis = std::stoll(frac);
			}
			return static_cast<int64_t>(epoch) * 1000 + millis;
		}
	}
	return NowMs();
}

} // namespace TimeUtil

#endif // OBS_MULTISTREAM_FRONTEND_TIME_UTIL_HPP_
