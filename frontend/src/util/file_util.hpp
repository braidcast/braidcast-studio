#ifndef OBS_MULTISTREAM_FRONTEND_FILE_UTIL_HPP_
#define OBS_MULTISTREAM_FRONTEND_FILE_UTIL_HPP_

#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>

// Whole-file reads. Kept in one place so every subsystem that asks "give me this
// file's bytes" gets the same answer, including for the awkward cases.
namespace FileUtil {

// Read an entire file in binary mode into `out` (a std::string or a char/byte
// vector). False only when the file could not be opened; `out` is then left
// untouched.
//
// There is deliberately no mid-read failure signal. istreambuf_iterator draws
// straight from the streambuf and never touches the istream's state bits, so
// checking bad() here would be checking a flag nothing sets; a read that dies
// partway simply stops early and comes back short. Reporting it would mean
// switching to istream::read and re-deriving the length, which no caller has
// needed.
//
// `path` is handed to std::ifstream unchanged, deliberately: a
// std::filesystem::path from u8path() names a UTF-8 path while a plain
// std::string names one in the native narrow encoding, and on Windows those stop
// being the same file as soon as the path leaves ASCII. Picking one here would
// silently redirect the callers that rely on the other.
template<typename PathT, typename OutT> bool ReadBinaryFile(const PathT &path, OutT &out)
{
	std::ifstream in(path, std::ios::in | std::ios::binary);
	if (!in) {
		return false;
	}
	out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return true;
}

// Read an entire file whose path is UTF-8 -- the form the config stores and the
// overlay asset dirs carry paths in. False only when the file could not be opened.
inline bool ReadUtf8File(const std::string &utf8Path, std::string &out)
{
	return ReadBinaryFile(std::filesystem::u8path(utf8Path), out);
}

// Same read, as an optional, for the callers that must tell "no such file" apart
// from "empty file" -- the self-tests snapshot and restore the user's real config
// files, and recreating one that was absent is not a restore.
inline std::optional<std::string> ReadUtf8File(const std::string &utf8Path)
{
	std::string contents;
	if (!ReadUtf8File(utf8Path, contents)) {
		return std::nullopt;
	}
	return contents;
}

} // namespace FileUtil

#endif // OBS_MULTISTREAM_FRONTEND_FILE_UTIL_HPP_
