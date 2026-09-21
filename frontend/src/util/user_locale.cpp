#include "user_locale.hpp"

#include "text_encoding.hpp"

#include <windows.h>

namespace UserLocale {

std::wstring NameW()
{
	wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
	if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) {
		return std::wstring();
	}
	return std::wstring(locale);
}

std::string UiLanguage()
{
	// A double-NUL-terminated list, most preferred first; only the first entry is wanted.
	ULONG count = 0;
	ULONG chars = 0;
	if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &chars) || chars == 0) {
		return std::string();
	}
	std::wstring list(chars, L'\0');
	if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, list.data(), &chars) || count == 0) {
		return std::string();
	}
	return Encoding::WideToUtf8(list.c_str());
}

} // namespace UserLocale
