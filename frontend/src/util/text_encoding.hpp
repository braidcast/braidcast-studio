#ifndef OBS_MULTISTREAM_FRONTEND_TEXT_ENCODING_HPP_
#define OBS_MULTISTREAM_FRONTEND_TEXT_ENCODING_HPP_

#include <windows.h>

#include <string>

// The UTF-8 <-> UTF-16 boundary. Everything the app holds in std::string is UTF-8;
// every Win32 *W call wants UTF-16. Both directions live here so the length and
// terminator arithmetic -- the part that is easy to get subtly wrong and hard to
// notice, because ASCII input hides the mistake -- is written once.
namespace Encoding {

// Empty (including a conversion the OS rejects) maps to an empty wstring.
inline std::wstring Utf8ToWide(const std::string &utf8)
{
	if (utf8.empty()) {
		return std::wstring();
	}
	// Length-counted rather than NUL-terminated, so an embedded NUL survives.
	const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
	if (len <= 0) {
		return std::wstring();
	}
	std::wstring out(static_cast<size_t>(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), len);
	return out;
}

// Null, empty, or unconvertible input maps to an empty string. The -1 length asks
// Win32 to include the terminator in its count, which is why the result is sized
// one short of it.
inline std::string WideToUtf8(const wchar_t *wide)
{
	if (!wide || !*wide) {
		return std::string();
	}
	const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1) {
		return std::string();
	}
	std::string out(static_cast<size_t>(len - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
	return out;
}

} // namespace Encoding

#endif // OBS_MULTISTREAM_FRONTEND_TEXT_ENCODING_HPP_
