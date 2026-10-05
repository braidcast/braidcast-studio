#include "fx_parse.hpp"

#include <cctype>
#include <cstdlib>
#include <string_view>

namespace Fx {

namespace {

// The value of attribute `name` in the tag that starts at `from`, quoted either way, or ""
// when the tag (up to its '>') has no such attribute.
std::string AttrValue(const std::string &xml, size_t from, const char *name)
{
	const size_t end = xml.find('>', from);
	const std::string key = std::string(name) + "=";
	size_t at = xml.find(key, from);
	if (at == std::string::npos || (end != std::string::npos && at > end)) {
		return std::string();
	}
	at += key.size();
	if (at >= xml.size() || (xml[at] != '\'' && xml[at] != '"')) {
		return std::string();
	}
	const char quote = xml[at];
	const size_t close = xml.find(quote, at + 1);
	if (close == std::string::npos) {
		return std::string();
	}
	return xml.substr(at + 1, close - at - 1);
}

} // namespace

bool IsCurrencyCode(const std::string &code)
{
	if (code.size() != 3) {
		return false;
	}
	for (const char c : code) {
		if (c < 'A' || c > 'Z') {
			return false;
		}
	}
	return true;
}

namespace {

// Currency -> the countries that use it, for the currencies the ECB publishes plus EUR.
// Data, not branches: a currency the ECB adds is one row.
struct CurrencyCountries {
	const char *currency;
	const char *countries; // ISO 3166-1 alpha-2 codes, space-separated
};

constexpr CurrencyCountries kCurrencyCountries[] = {
	{"EUR", "AT BE BG HR CY EE FI FR DE GR IE IT LV LT LU MT NL PT SK SI ES AD MC SM VA ME XK GF GP MQ RE "
		"YT PM BL MF AX"},
	{"USD", "US PR GU VI AS MP UM EC SV TL FM MH PW BQ VG TC ZW"},
	{"JPY", "JP"},
	{"CZK", "CZ"},
	{"DKK", "DK GL FO"},
	{"GBP", "GB IM JE GG"},
	{"HUF", "HU"},
	{"PLN", "PL"},
	{"RON", "RO"},
	{"SEK", "SE"},
	{"CHF", "CH LI"},
	{"ISK", "IS"},
	{"NOK", "NO SJ BV"},
	{"TRY", "TR"},
	{"AUD", "AU CX CC NF KI NR TV"},
	{"BRL", "BR"},
	{"CAD", "CA"},
	{"CNY", "CN"},
	{"HKD", "HK"},
	{"IDR", "ID"},
	{"ILS", "IL"},
	{"INR", "IN"},
	{"KRW", "KR"},
	{"MXN", "MX"},
	{"MYR", "MY"},
	{"NZD", "NZ CK NU PN TK"},
	{"PHP", "PH"},
	{"SGD", "SG"},
	{"THB", "TH"},
	{"ZAR", "ZA"},
};

} // namespace

bool ParseEcbDaily(const std::string &xml, RateTable &out)
{
	RateTable table;
	for (size_t at = xml.find("<Cube"); at != std::string::npos; at = xml.find("<Cube", at + 5)) {
		const std::string time = AttrValue(xml, at, "time");
		if (!time.empty()) {
			if (!table.date.empty() && table.date != time) {
				return false; // a history feed, not the daily one
			}
			table.date = time;
			continue;
		}
		const std::string currency = AttrValue(xml, at, "currency");
		const std::string rate = AttrValue(xml, at, "rate");
		if (currency.empty() || rate.empty()) {
			continue;
		}
		char *end = nullptr;
		const double value = std::strtod(rate.c_str(), &end);
		if (!IsCurrencyCode(currency) || end == rate.c_str() || *end != '\0' || !(value > 0.0)) {
			return false;
		}
		table.perEur[currency] = value;
	}
	if (DaysFromIsoDate(table.date) < 0 || table.perEur.size() < kMinRates) {
		return false;
	}
	out = std::move(table);
	return true;
}

std::string CurrencyForCountry(const std::string &country)
{
	if (country.size() != 2) {
		return std::string();
	}
	std::string code;
	for (const char c : country) {
		code += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	for (const CurrencyCountries &row : kCurrencyCountries) {
		std::string_view list(row.countries);
		for (size_t at = 0; at + 2 <= list.size(); at += 3) {
			if (list.substr(at, 2) == code) {
				return row.currency;
			}
		}
	}
	return std::string();
}

std::string RegionOfLocale(const std::string &locale)
{
	// Subtags after the language: a 4-letter script, then the region (2 letters, or 3 digits
	// for a UN area, which names no country). The first 2-letter one is the region.
	size_t start = 0;
	bool first = true;
	while (start <= locale.size()) {
		size_t end = locale.find_first_of("-_", start);
		if (end == std::string::npos) {
			end = locale.size();
		}
		const std::string part = locale.substr(start, end - start);
		if (!first && part.size() == 2 && std::isalpha(static_cast<unsigned char>(part[0])) &&
		    std::isalpha(static_cast<unsigned char>(part[1]))) {
			std::string region;
			for (const char c : part) {
				region += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			}
			return region;
		}
		first = false;
		start = end + 1;
	}
	return std::string();
}

HomeChoice ResolveHome(const std::string &setting, const std::vector<std::string> &youtubeCountries,
		       const std::string &localeRegion)
{
	if (IsCurrencyCode(setting)) {
		return {setting, "setting"};
	}
	for (const std::string &country : youtubeCountries) {
		const std::string code = CurrencyForCountry(country);
		if (!code.empty()) {
			return {code, "youtube"};
		}
	}
	const std::string code = CurrencyForCountry(localeRegion);
	if (!code.empty()) {
		return {code, "locale"};
	}
	return {std::string(), "none"};
}

int64_t DaysFromIsoDate(const std::string &date)
{
	if (date.size() != 10 || date[4] != '-' || date[7] != '-') {
		return -1;
	}
	for (const size_t i : {0, 1, 2, 3, 5, 6, 8, 9}) {
		if (!std::isdigit(static_cast<unsigned char>(date[i]))) {
			return -1;
		}
	}
	int64_t y = std::atoi(date.substr(0, 4).c_str());
	const unsigned m = static_cast<unsigned>(std::atoi(date.substr(5, 2).c_str()));
	const unsigned d = static_cast<unsigned>(std::atoi(date.substr(8, 2).c_str()));
	if (m < 1 || m > 12 || d < 1 || d > 31) {
		return -1;
	}
	// Howard Hinnant's days_from_civil.
	y -= m <= 2 ? 1 : 0;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

} // namespace Fx
