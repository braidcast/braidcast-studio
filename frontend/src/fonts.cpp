#include "fonts.hpp"

#include "log.hpp"
#include "util/string_util.hpp"
#include "util/text_encoding.hpp"

#include <util/base.h>

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>
#endif

namespace Fonts {
namespace {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

// One family's name in the locale the user reads. A family carries a name per locale it
// was localized for, so the preference order is the user's own locale, then "en-us"
// (which every family DirectWrite ships resolves for), then whatever sits at index 0 --
// a family always has at least one name, but not necessarily one in either locale.
std::string FamilyName(IDWriteFontFamily *family)
{
	ComPtr<IDWriteLocalizedStrings> names;
	if (FAILED(family->GetFamilyNames(names.GetAddressOf())) || names->GetCount() == 0) {
		return std::string();
	}

	UINT32 index = 0;
	BOOL exists = FALSE;
	wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
	if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) != 0) {
		names->FindLocaleName(locale, &index, &exists);
	}
	if (!exists) {
		names->FindLocaleName(L"en-us", &index, &exists);
	}
	if (!exists) {
		index = 0;
	}

	UINT32 length = 0;
	if (FAILED(names->GetStringLength(index, &length)) || length == 0) {
		return std::string();
	}
	// GetString writes the terminator too, so the buffer is one wider than the length
	// reported; the wstring is then trimmed back to the characters themselves.
	std::wstring name(static_cast<size_t>(length) + 1, L'\0');
	if (FAILED(names->GetString(index, name.data(), length + 1))) {
		return std::string();
	}
	name.resize(length);
	return Encoding::WideToUtf8(name.c_str());
}
#endif

std::vector<std::string> Enumerate()
{
#ifdef _WIN32
	// No CoInitialize: DirectWrite is not COM-activated -- DWriteCreateFactory is a
	// plain export that builds the factory itself, so no apartment has to exist on the
	// calling thread.
	ComPtr<IDWriteFactory> factory;
	HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
					 reinterpret_cast<IUnknown **>(factory.GetAddressOf()));
	if (FAILED(hr)) {
		blog(LOG_WARNING, "Fonts: DWriteCreateFactory failed (hr=0x%08lX)", hr);
		return std::vector<std::string>();
	}

	// FALSE: do not force a re-scan of the system collection. A rescan would stall
	// this call for a list that is only ever a suggestion.
	ComPtr<IDWriteFontCollection> collection;
	hr = factory->GetSystemFontCollection(collection.GetAddressOf(), FALSE);
	if (FAILED(hr)) {
		blog(LOG_WARNING, "Fonts: GetSystemFontCollection failed (hr=0x%08lX)", hr);
		return std::vector<std::string>();
	}

	const UINT32 count = collection->GetFontFamilyCount();
	std::vector<std::string> names;
	names.reserve(count);
	for (UINT32 i = 0; i < count; ++i) {
		ComPtr<IDWriteFontFamily> family;
		if (FAILED(collection->GetFontFamily(i, family.GetAddressOf()))) {
			continue;
		}
		std::string name = FamilyName(family.Get());
		if (!name.empty()) {
			names.push_back(std::move(name));
		}
	}

	// Sorted and deduplicated the way the reader sees the names, not the way bytes
	// compare: a collection can offer one family under two casings, and "Arial" filed
	// apart from "arial" reads as two fonts.
	std::sort(names.begin(), names.end(), StringUtil::LessCI);
	names.erase(std::unique(names.begin(), names.end(), StringUtil::EqualsCI), names.end());
	DBG(LogCat::Bridge, "fonts: enumerated %zu families", names.size());
	return names;
#else
	return std::vector<std::string>();
#endif
}

} // namespace

const std::vector<std::string> &ListFamilies()
{
	static const std::vector<std::string> families = Enumerate();
	return families;
}

} // namespace Fonts
