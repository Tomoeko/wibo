#pragma once

#include "common.h"
#include "kernel32/internal.h"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

using kernel32::FileObject;

namespace files {

struct FileShareRegistry;

// Admission is process-local and covers opens through CreateFile. It does not
// establish sharing policy for imported descriptors or mapped sections.
class FileOpenAdmission {
  public:
	FileOpenAdmission();
	void releaseForStreamOpen();
	DWORD admit(kernel32::FsObject &file, uint32_t grantedAccess, uint32_t sharing, bool truncate);

  private:
	std::shared_ptr<FileShareRegistry> mRegistry;
	std::unique_lock<std::mutex> mLock;
};

struct IOResult {
	size_t bytesTransferred = 0;
	int unixError = 0;
	DWORD windowsError = 0;
	bool reachedEnd = false;
};

struct StandardHandles {
	HANDLE input;
	HANDLE output;
	HANDLE error;
	bool explicitStartup = true;
};

struct SystemSearchDirectories {
	std::filesystem::path system;
	std::filesystem::path legacySystem;
	std::filesystem::path windows;
};

bool isNullDevice(const FileObject &file);

DWORD lockRange(FileObject *file, uint64_t start, uint64_t length, bool exclusive, bool blocking);
DWORD unlockRange(FileObject *file, uint64_t start, uint64_t length);
DWORD checkRangeAccess(FileObject *file, off_t start, size_t length, bool writing);

void init(std::optional<StandardHandles> inheritedStandards = std::nullopt);
// The caller must retain the object and hold file.m for these operations.
DWORD prepareInheritanceLocked(FileObject &file);
DWORD queryPositionLocked(FileObject &file, off_t &position);
DWORD seekPositionLocked(FileObject &file, int64_t distance, DWORD method, off_t &position,
						 uint64_t maximumPosition = INT64_MAX);
DWORD truncateAtPositionLocked(FileObject &file);
std::filesystem::path pathFromWindows(const char *inStr);
SystemSearchDirectories systemSearchDirectories();
std::string pathToWindows(const std::filesystem::path &path);
// The caller must retain the file object for the complete I/O operation.
IOResult read(FileObject *file, void *buffer, size_t bytesToRead, const std::optional<off_t> &offset,
			  bool updateFilePointer);
IOResult write(FileObject *file, const void *buffer, size_t bytesToWrite, const std::optional<off_t> &offset,
			   bool updateFilePointer);
HANDLE getStdHandle(DWORD nStdHandle);
BOOL setStdHandle(DWORD nStdHandle, HANDLE hHandle);
// The startup flag is fixed at initialization; selected handle values remain current.
std::optional<StandardHandles> startupStandardHandles();
std::optional<std::filesystem::path> findCaseInsensitiveFile(const std::filesystem::path &directory,
															 const std::string &filename);
std::filesystem::path canonicalPath(const std::filesystem::path &path);
std::string hostPathListToWindows(const std::string &value);
std::string windowsPathListToHost(const std::string &value);

} // namespace files

inline bool endsWith(const std::string &str, const std::string &suffix) {
	return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}
