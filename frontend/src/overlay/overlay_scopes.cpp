#include "overlay_scopes.hpp"

#include "../log.hpp"
#include "util/file_util.hpp"
#include "util/web_bundle.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>

namespace Overlay {

namespace {

constexpr size_t kMaxLibraryPathBytes = 256;
constexpr int kMaxLibraryDepth = 2;
constexpr char kLibraryManifest[] = "sounds.json";

std::mutex g_manifestMutex;
// id -> file, once the manifest has read. Never invalidated: the library is staged into the
// rundir by the build, so it cannot change under a running app.
std::optional<std::map<std::string, std::string>> g_manifest;

bool IsLibraryNameChar(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
	       c == '-';
}

// The manifest as id -> file, or nullopt when it does not read or parse. An entry whose file
// would not pass IsSafeLibraryPath is left out, so a bad row cannot become a served URL.
std::optional<std::map<std::string, std::string>> ReadManifest()
{
	std::string text;
	if (!FileUtil::ReadUtf8File(LibraryRoot() + "/" + kLibraryManifest, text)) {
		return std::nullopt;
	}
	const json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
	if (!parsed.is_array()) {
		return std::nullopt;
	}
	std::map<std::string, std::string> out;
	for (const json &row : parsed) {
		if (!row.is_object()) {
			continue;
		}
		const std::string id = row.value("id", std::string());
		const std::string file = row.value("file", std::string());
		if (!id.empty() && IsSafeLibraryPath(file)) {
			out.emplace(id, file);
		}
	}
	return out;
}

} // namespace

std::string LibraryRoot()
{
	return WebBundle::Root() + "/overlay/library";
}

bool IsSafeLibraryPath(const std::string &rel)
{
	if (rel.empty() || rel.size() > kMaxLibraryPathBytes) {
		return false;
	}
	int depth = 0;
	size_t start = 0;
	while (start <= rel.size()) {
		const size_t slash = rel.find('/', start);
		const std::string segment =
			rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
		if (segment.empty() || segment.front() == '.' || ++depth > kMaxLibraryDepth) {
			return false;
		}
		for (char c : segment) {
			if (!IsLibraryNameChar(c)) {
				return false;
			}
		}
		if (slash == std::string::npos) {
			break;
		}
		start = slash + 1;
	}
	// The character rules already make an escape impossible; this is the belt to them, so
	// a later loosening of the rules cannot quietly serve a file from outside the library.
	std::error_code ec;
	const std::filesystem::path root =
		std::filesystem::weakly_canonical(std::filesystem::u8path(LibraryRoot()), ec);
	if (ec) {
		return false;
	}
	const std::filesystem::path full = std::filesystem::weakly_canonical(root / std::filesystem::u8path(rel), ec);
	if (ec) {
		return false;
	}
	const auto mismatch = std::mismatch(root.begin(), root.end(), full.begin(), full.end());
	return mismatch.first == root.end();
}

std::optional<std::string> LibraryFileFor(const std::string &id)
{
	std::lock_guard<std::mutex> lock(g_manifestMutex);
	if (!g_manifest) {
		g_manifest = ReadManifest();
		if (!g_manifest) {
			HostLog("[overlay] the sound library manifest at " + LibraryRoot() + "/" + kLibraryManifest +
				" did not read; library sounds are not served");
			return std::nullopt;
		}
	}
	const auto it = g_manifest->find(id);
	if (it == g_manifest->end()) {
		return std::nullopt;
	}
	return it->second;
}

} // namespace Overlay
