#ifndef OBS_MULTISTREAM_FRONTEND_FILE_UTIL_HPP_
#define OBS_MULTISTREAM_FRONTEND_FILE_UTIL_HPP_

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>

// Whole-file reads, and a crash-safe whole-file write of raw bytes. Kept in one place so every
// subsystem that asks "give me this file's bytes" gets the same answer, including for the
// awkward cases.
namespace FileUtil {

// Which step of WriteBinaryFileAtomic failed, so each caller keeps its own log lines.
enum class AtomicWriteResult { Ok, OpenFailed, WriteFailed, ReplaceFailed };

// Replace the file at the UTF-8 path `utf8Path` with `size` bytes from `data`, so that a
// crash or power loss leaves either the old contents or the new ones, never a truncated
// mix. The bytes go to "<path>.tmp", are flushed to disk, and are then swapped in with
// os_safe_replace, which rides out another process briefly holding the target open.
//
// No backup copy is kept. For the OAuth token store a backup would keep a removed
// account's wrapped tokens on disk after the removal was saved.
//
// A failed write or replace removes the temp file, and the previous file is normally still
// in place. The exception is a replace that fails while nothing is left under the target
// name (a first save, or a replace that already removed the previous file): it keeps the
// temp file, because that then holds the only copy of the data. A caller that can recover
// from it reads AtomicWriteTempPath when the target is missing; one that cannot deletes it.
// OpenFailed touches nothing, so a temp file an earlier call kept is still there.
AtomicWriteResult WriteBinaryFileAtomic(const std::string &utf8Path, const void *data, size_t size);

// The temp file WriteBinaryFileAtomic writes `utf8Path` through, as a UTF-8 path.
std::string AtomicWriteTempPath(const std::string &utf8Path);

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
