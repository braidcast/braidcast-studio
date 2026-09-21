#ifndef OBS_MULTISTREAM_FRONTEND_UTIL_USER_LOCALE_HPP_
#define OBS_MULTISTREAM_FRONTEND_UTIL_USER_LOCALE_HPP_

#include <string>

// The user's Windows locale settings. Windows keeps two that are easy to confuse, and they
// often disagree -- an English UI with Russian date and number formats is ordinary:
//
// - NameW(): the regional-format locale (Settings > Region > Regional format). Right for
//   formatting and for picking a font family's localized name.
// - UiLanguage(): the display language the user reads Windows in. Right for guessing what
//   language the user speaks. Never use NameW() for that.
//
// Both are BCP-47 names ("en-US", "zh-Hant-TW"), or empty when the OS will not say.
namespace UserLocale {

std::wstring NameW();
std::string UiLanguage();

} // namespace UserLocale

#endif // OBS_MULTISTREAM_FRONTEND_UTIL_USER_LOCALE_HPP_
