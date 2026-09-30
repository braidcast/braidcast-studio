#include "file_util.hpp"

#include <filesystem>

#include <windows.h>

#include <util/platform.h>

namespace FileUtil {

namespace {

// Write every byte, then flush them to disk. ReplaceFileW has no write-through mode, so the
// data is made durable here, before the replace publishes it under the real name -- the
// order Chromium's ImportantFileWriter uses.
bool WriteAllAndFlush(HANDLE file, const unsigned char *bytes, size_t size)
{
	while (size > 0) {
		const DWORD chunk = size > MAXDWORD ? MAXDWORD : static_cast<DWORD>(size);
		DWORD written = 0;
		if (!WriteFile(file, bytes, chunk, &written, nullptr) || written == 0) {
			return false;
		}
		bytes += written;
		size -= written;
	}
	return FlushFileBuffers(file) != 0;
}

} // namespace

std::string AtomicWriteTempPath(const std::string &utf8Path)
{
	return utf8Path + ".tmp";
}

AtomicWriteResult WriteBinaryFileAtomic(const std::string &utf8Path, const void *data, size_t size)
{
	const std::string tmpUtf8 = AtomicWriteTempPath(utf8Path);
	const std::filesystem::path tmpPath = std::filesystem::u8path(tmpUtf8);

	const HANDLE file =
		CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return AtomicWriteResult::OpenFailed;
	}
	const bool written = WriteAllAndFlush(file, static_cast<const unsigned char *>(data), size);
	CloseHandle(file);
	if (!written) {
		DeleteFileW(tmpPath.c_str());
		return AtomicWriteResult::WriteFailed;
	}

	if (os_safe_replace(utf8Path.c_str(), tmpUtf8.c_str(), nullptr) != 0) {
		// Without a backup name, ReplaceFileW's ERROR_UNABLE_TO_MOVE_REPLACEMENT deletes the
		// previous file and leaves the new bytes only under the temp name. Deleting the temp
		// then would lose both, so it is removed only while the target still exists.
		if (GetFileAttributesW(std::filesystem::u8path(utf8Path).c_str()) != INVALID_FILE_ATTRIBUTES) {
			DeleteFileW(tmpPath.c_str());
		}
		return AtomicWriteResult::ReplaceFailed;
	}
	return AtomicWriteResult::Ok;
}

} // namespace FileUtil
