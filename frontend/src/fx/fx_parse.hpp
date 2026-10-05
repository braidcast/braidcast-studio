#ifndef OBS_MULTISTREAM_FRONTEND_FX_FX_PARSE_HPP_
#define OBS_MULTISTREAM_FRONTEND_FX_FX_PARSE_HPP_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The pure half of the exchange-rate store (fx_rates): reading the European Central Bank's
// daily reference rates, and naming a currency for a country or a locale. Standard library
// only, so it is checked the same way on every platform.
namespace Fx {

// One day's ECB reference rates: units of each currency per 1 EUR, as published. EUR itself
// is not listed in the feed and is implied at 1.
struct RateTable {
	std::string date; // "YYYY-MM-DD", the ECB's reference date
	std::map<std::string, double> perEur;
};

// The fewest currencies a feed must carry to be believed. The ECB publishes about 30; a
// page that parses to fewer is an error page or a truncated body, and replacing a good
// cached table with it would turn every conversion off.
inline constexpr size_t kMinRates = 10;

// Parse eurofxref-daily.xml. False, leaving `out` untouched, unless the body holds one
// reference date and at least kMinRates positive rates under three-letter codes.
bool ParseEcbDaily(const std::string &xml, RateTable &out);

// The currency a country (ISO 3166-1 alpha-2, any case) uses, among the currencies the ECB
// publishes plus EUR; "" for any other country. A home currency outside that set could not
// be converted to anyway.
std::string CurrencyForCountry(const std::string &country);

// The region subtag of a BCP-47 locale ("en-IN" -> "IN", "zh-Hant-TW" -> "TW"), upper-cased;
// "" when it names none ("en", "").
std::string RegionOfLocale(const std::string &locale);

// Days since 1970-01-01 for a "YYYY-MM-DD" date; -1 when it is not one.
int64_t DaysFromIsoDate(const std::string &date);

// Whether `code` is a three-letter upper-case currency code.
bool IsCurrencyCode(const std::string &code);

// The streamer's currency and where it came from: "setting" (chosen in Settings),
// "youtube" (the connected YouTube channel's country), "locale" (the Windows regional
// format's region) or "none" (code empty: nothing named one the rates can convert to).
struct HomeChoice {
	std::string code;
	std::string source;
};

// The first of: the setting when it is a currency code; the first YouTube channel country
// that maps to a currency; the locale region's currency.
HomeChoice ResolveHome(const std::string &setting, const std::vector<std::string> &youtubeCountries,
		       const std::string &localeRegion);

} // namespace Fx

#endif // OBS_MULTISTREAM_FRONTEND_FX_FX_PARSE_HPP_
