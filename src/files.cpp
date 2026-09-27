#include "files.h"
#include "common.h"
#include "errors.h"
#include "handles.h"
#include "kernel32/fileapi.h"
#include "kernel32/winbase.h"
#include "strutil.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace files {

struct FileShareRegistry {
	std::mutex mutex;
	std::vector<const FileShareLease *> opens;
};

struct FileShareLease {
	std::shared_ptr<FileShareRegistry> registry;
	dev_t device;
	ino_t inode;
	uint32_t access;
	uint32_t sharing;
	bool registered = false;

	FileShareLease(std::shared_ptr<FileShareRegistry> owner, const struct stat &metadata, uint32_t access,
				   uint32_t sharing)
		: registry(std::move(owner)), device(metadata.st_dev), inode(metadata.st_ino), access(access),
		  sharing(sharing) {}
	~FileShareLease();
};

} // namespace files

namespace {

std::shared_ptr<files::FileShareRegistry> fileShareRegistry() {
	// Leases keep the registry alive while filesystem objects are destroyed.
	static const auto registry = std::make_shared<files::FileShareRegistry>();
	return registry;
}

uint32_t sharingAccess(uint32_t grantedAccess) {
	uint32_t access = 0;
	if (grantedAccess & (FILE_READ_DATA | FILE_EXECUTE))
		access |= FILE_SHARE_READ;
	if (grantedAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA))
		access |= FILE_SHARE_WRITE;
	if (grantedAccess & DELETE)
		access |= FILE_SHARE_DELETE;
	return access;
}

void removeFileShareLocked(files::FileShareLease &lease) {
	if (!lease.registered)
		return;
	std::erase(lease.registry->opens, &lease);
	lease.registered = false;
}

int cursorRecordLock(int fd, short type) {
	if (fd < 0)
		return 0;
	struct flock request{};
	request.l_type = type;
	request.l_whence = SEEK_SET;
	request.l_len = 1;
	int result;
	do {
		result = fcntl(fd, type == F_UNLCK ? F_SETLK : F_SETLKW, &request);
	} while (result == -1 && errno == EINTR);
	return result == 0 ? 0 : (errno ? errno : EIO);
}

class CursorOperation {
  public:
	explicit CursorOperation(kernel32::FileCursor &cursor) : mCursor(cursor), mError(cursor.lockOperationLocked()) {}
	CursorOperation(const CursorOperation &) = delete;
	CursorOperation &operator=(const CursorOperation &) = delete;
	~CursorOperation() {
		if (!mFinished && !mError) {
			const int error = mCursor.unlockOperationLocked();
			if (error)
				DEBUG_LOG("Cursor unlock failed: errno=%d\n", error);
		}
	}
	[[nodiscard]] int error() const { return mError; }
	int finish() {
		mFinished = true;
		return mError ? mError : mCursor.unlockOperationLocked();
	}

  private:
	kernel32::FileCursor &mCursor;
	int mError;
	bool mFinished = false;
};

} // namespace

files::FileShareLease::~FileShareLease() {
	if (registered) {
		std::lock_guard lock(registry->mutex);
		removeFileShareLocked(*this);
	}
}

files::FileOpenAdmission::FileOpenAdmission() : mRegistry(fileShareRegistry()), mLock(mRegistry->mutex) {}

void files::FileOpenAdmission::releaseForStreamOpen() { mLock.unlock(); }

DWORD files::FileOpenAdmission::admit(kernel32::FsObject &file, uint32_t grantedAccess, uint32_t sharing,
									  bool truncate) {
	if (!mLock.owns_lock())
		mLock.lock();
	struct stat metadata{};
	if (fstat(file.fd, &metadata) != 0)
		return wibo::winErrorFromErrno(errno);
	const uint32_t access = sharingAccess(grantedAccess);
	std::shared_ptr<FileShareLease> lease;
	if (S_ISREG(metadata.st_mode) || S_ISDIR(metadata.st_mode)) {
		for (const auto *existing : mRegistry->opens) {
			if (existing->device != metadata.st_dev || existing->inode != metadata.st_ino || !existing->access ||
				!access)
				continue;
			if ((access & ~existing->sharing) || (existing->access & ~sharing))
				return ERROR_SHARING_VIOLATION;
		}
		lease = std::make_shared<FileShareLease>(mRegistry, metadata, access, sharing);
		// Allocate the registry entry before making a destructive change.
		mRegistry->opens.push_back(lease.get());
	}
	if (truncate && S_ISREG(metadata.st_mode) && ftruncate(file.fd, 0) != 0) {
		const DWORD error = wibo::winErrorFromErrno(errno);
		if (lease)
			mRegistry->opens.pop_back();
		return error;
	}
	if (lease) {
		lease->registered = true;
		file.shareLease = std::move(lease);
	}
	// The lease reserves this open before handle allocation can release its owner.
	mLock.unlock();
	return ERROR_SUCCESS;
}

kernel32::FileCursor::~FileCursor() {
	if (mControlFd >= 0)
		close(mControlFd);
}

int kernel32::FileCursor::prepareTransferLocked() {
	if (mControlFd >= 0)
		return 0;
	const char *temporaryDirectory = std::getenv("TMPDIR");
	std::string path = temporaryDirectory && temporaryDirectory[0] ? temporaryDirectory : "/tmp";
	if (path.back() != '/')
		path.push_back('/');
	path += "wibo-cursor-XXXXXX";
	std::vector<char> name(path.begin(), path.end());
	name.push_back('\0');
	const int fd = mkostemp(name.data(), O_CLOEXEC);
	if (fd == -1)
		return errno ? errno : EIO;
	if (unlink(name.data()) == -1) {
		const int error = errno ? errno : EIO;
		close(fd);
		return error;
	}
	mControlFd = fd;
	return 0;
}

int kernel32::FileCursor::adoptControlDescriptor(int fd) {
	if (mControlFd >= 0 || fd < 0)
		return EINVAL;
	struct stat metadata{};
	if (fstat(fd, &metadata) == -1)
		return errno ? errno : EIO;
	const int flags = fcntl(fd, F_GETFL);
	if (flags == -1)
		return errno ? errno : EIO;
	if (!S_ISREG(metadata.st_mode) || (flags & O_ACCMODE) != O_RDWR)
		return EINVAL;
	const int descriptorFlags = fcntl(fd, F_GETFD);
	if (descriptorFlags == -1 || fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC) == -1)
		return errno ? errno : EIO;
	mControlFd = fd;
	return 0;
}

int kernel32::FileCursor::lockOperationLocked() const { return cursorRecordLock(mControlFd, F_WRLCK); }
int kernel32::FileCursor::unlockOperationLocked() const { return cursorRecordLock(mControlFd, F_UNLCK); }

kernel32::FileObject::~FileObject() {
	for (auto iter = ownedDescriptorAliases.begin(); iter != ownedDescriptorAliases.end(); ++iter) {
		if (*iter >= 0 && *iter != fd && std::find(ownedDescriptorAliases.begin(), iter, *iter) == iter)
			close(*iter);
	}
}

kernel32::FsObject::~FsObject() {
	// Native close and share removal are one operation relative to admission.
	auto share = std::move(shareLease);
	std::unique_lock<std::mutex> shareLock;
	if (share)
		shareLock = std::unique_lock(share->registry->mutex);
	int fd = std::exchange(this->fd, -1);
	if (fd >= 0 && closeOnDestroy) {
		close(fd);
	}
	if (deletePending && !canonicalPath.empty()) {
		if (unlink(canonicalPath.c_str()) != 0) {
			perror("Failed to delete file on close");
		}
	}
	if (share)
		removeFileShareLocked(*share);
}

namespace files {

DWORD prepareInheritanceLocked(FileObject &file) {
	if (!file.valid())
		return ERROR_INVALID_HANDLE;
	if (file.type != ObjectType::File || file.overlapped || std::atomic_load(&file.completion) || file.deletePending ||
		!file.rangeLocks.empty() || file.pipeMessageMode)
		return ERROR_NOT_SUPPORTED;
	struct stat metadata{};
	if (fstat(file.fd, &metadata) == -1)
		return wibo::winErrorFromErrno(errno);
	if (!S_ISREG(metadata.st_mode) && !S_ISFIFO(metadata.st_mode) && !S_ISCHR(metadata.st_mode))
		return ERROR_NOT_SUPPORTED;
	// A private control inode serializes compound offset operations after transfer.
	// Closing any duplicate of this inode must also hold the file object's mutex.
	if (S_ISREG(metadata.st_mode))
		return wibo::winErrorFromErrno(file.cursor.prepareTransferLocked());
	return ERROR_SUCCESS;
}

bool isNullDevice(const FileObject &file) {
	struct stat descriptor{}, device{};
	return file.valid() && fstat(file.fd, &descriptor) == 0 && S_ISCHR(descriptor.st_mode) &&
		   stat("/dev/null", &device) == 0 && descriptor.st_rdev == device.st_rdev;
}

DWORD queryPositionLocked(FileObject &file, off_t &position) {
	// The host null device does not retain a shared logical cursor.
	if (isNullDevice(file))
		return ERROR_NOT_SUPPORTED;
	CursorOperation operation(file.cursor);
	if (operation.error())
		return wibo::winErrorFromErrno(operation.error());
	const off_t current = lseek(file.fd, 0, SEEK_CUR);
	int error = current == -1 ? (errno ? errno : EIO) : 0;
	const int unlockError = operation.finish();
	if (!error)
		error = unlockError;
	if (error)
		return wibo::winErrorFromErrno(error);
	position = current;
	return ERROR_SUCCESS;
}

DWORD seekPositionLocked(FileObject &file, int64_t distance, DWORD method, off_t &position, uint64_t maximumPosition) {
	if (isNullDevice(file))
		return ERROR_NOT_SUPPORTED;
	if (method != FILE_BEGIN && method != FILE_CURRENT && method != FILE_END)
		return ERROR_INVALID_PARAMETER;
	if (file.isPipe)
		return ERROR_INVALID_PARAMETER;
	CursorOperation operation(file.cursor);
	if (operation.error())
		return wibo::winErrorFromErrno(operation.error());
	off_t base = 0;
	DWORD error = ERROR_SUCCESS;
	if (method == FILE_CURRENT) {
		base = lseek(file.fd, 0, SEEK_CUR);
		if (base == -1)
			error = wibo::winErrorFromErrno(errno);
	} else if (method == FILE_END) {
		struct stat metadata{};
		if (fstat(file.fd, &metadata) == -1)
			error = wibo::winErrorFromErrno(errno);
		else
			base = metadata.st_size;
	}
	off_t result = 0;
	if (!error) {
		if (base < 0 || (distance < 0 && distance < -static_cast<int64_t>(base))) {
			error = ERROR_NEGATIVE_SEEK;
		} else if (distance > 0 && distance > std::numeric_limits<off_t>::max() - base) {
			error = ERROR_INVALID_PARAMETER;
		} else {
			result = base + static_cast<off_t>(distance);
			if (static_cast<uint64_t>(result) > maximumPosition)
				error = ERROR_INVALID_PARAMETER;
			else if (lseek(file.fd, result, SEEK_SET) == -1)
				error = wibo::winErrorFromErrno(errno);
		}
	}
	const int unlockError = operation.finish();
	if (!error)
		error = wibo::winErrorFromErrno(unlockError);
	if (!error)
		position = result;
	return error;
}

DWORD truncateAtPositionLocked(FileObject &file) {
	if (isNullDevice(file))
		return ERROR_NOT_SUPPORTED;
	if (file.isPipe)
		return ERROR_INVALID_PARAMETER;
	CursorOperation operation(file.cursor);
	if (operation.error())
		return wibo::winErrorFromErrno(operation.error());
	const off_t position = lseek(file.fd, 0, SEEK_CUR);
	int error = 0;
	if (position == -1 || ftruncate(file.fd, position) == -1)
		error = errno ? errno : EIO;
	const int unlockError = operation.finish();
	return wibo::winErrorFromErrno(error ? error : unlockError);
}

static std::vector<std::string> splitList(const std::string &value, char delimiter) {
	std::vector<std::string> entries;
	size_t start = 0;
	while (start <= value.size()) {
		size_t end = value.find(delimiter, start);
		if (end == std::string::npos) {
			end = value.size();
		}
		entries.emplace_back(value.substr(start, end - start));
		if (end == value.size()) {
			break;
		}
		start = end + 1;
	}
	return entries;
}

static std::string toWindowsPathEntry(const std::string &entry) {
	if (entry.empty()) {
		return {};
	}
	bool looksWindows =
		entry.find('\\') != std::string::npos || (entry.size() >= 2 && entry[1] == ':' && entry[0] != '/');
	if (looksWindows) {
		std::string normalized = entry;
		std::replace(normalized.begin(), normalized.end(), '/', '\\');
		return normalized;
	}
	return pathToWindows(std::filesystem::path(entry));
}

static std::string toHostPathEntry(const std::string &entry) {
	if (entry.empty()) {
		return {};
	}
	auto converted = pathFromWindows(entry.c_str());
	if (!converted.empty()) {
		return converted.string();
	}
	std::string normalized = entry;
	std::replace(normalized.begin(), normalized.end(), '\\', '/');
	return normalized;
}

static std::atomic<HANDLE> stdinHandle;
static std::atomic<HANDLE> stdoutHandle;
static std::atomic<HANDLE> stderrHandle;
static bool explicitStartupStandards = false;

// Strip the Windows trailing-dot "no extension" convention from each path
// component. Windows treats "foo." and "foo" as the same filename — the
// trailing dot means "no extension" — and many NT-era tools rely on that
// equivalence (notably NMAKE's @<< temp response files like "nm12345."
// and older makefile directives like "!INCLUDE .\sources."). Linux
// filesystems rarely carry a literal trailing-dot name, so normalize
// before our lookup / case-insensitive fallback runs.
static std::string stripTrailingDots(const std::string &s) {
	std::string out;
	out.reserve(s.size());
	size_t i = 0;
	while (i < s.size()) {
		size_t start = i;
		while (i < s.size() && s[i] != '/') {
			i++;
		}
		size_t end = i;
		size_t len = end - start;
		// Leave "." and ".." untouched.
		bool isDotDir = (len == 1 && s[start] == '.') || (len == 2 && s[start] == '.' && s[start + 1] == '.');
		if (!isDotDir) {
			while (end > start && s[end - 1] == '.') {
				end--;
			}
		}
		out.append(s, start, end - start);
		if (i < s.size()) {
			out.push_back('/');
			i++;
		}
	}
	return out;
}

std::filesystem::path pathFromWindows(const char *inStr) {
	// Convert to forward slashes
	std::string str = inStr;
	std::replace(str.begin(), str.end(), '\\', '/');

	// Remove "//?/" prefix
	if (str.rfind("//?/", 0) == 0) {
		str.erase(0, 4);
	}

	std::filesystem::path driveRoot;
	// Z: exposes the host root. C: keeps the historical host-root default, but
	// callers may provide an isolated Windows drive for tools with hard-coded
	// absolute paths via WIBO_C_DRIVE.
	if (str.rfind("z:/", 0) == 0 || str.rfind("Z:/", 0) == 0) {
		str.erase(0, 2);
	} else if (str.rfind("c:/", 0) == 0 || str.rfind("C:/", 0) == 0) {
		str.erase(0, 2);
		if (const char *configuredRoot = std::getenv("WIBO_C_DRIVE"); configuredRoot && configuredRoot[0]) {
			driveRoot = configuredRoot;
			while (!str.empty() && str.front() == '/') {
				str.erase(str.begin());
			}
		}
	}

	// Apply Windows trailing-dot normalization per path component.
	str = stripTrailingDots(str);

	// Return as-is if it exists, else traverse the filesystem looking for
	// a path that matches case insensitively
	std::filesystem::path path = driveRoot.empty() ? std::filesystem::path(str).lexically_normal()
												   : (driveRoot / std::filesystem::path(str)).lexically_normal();
	std::error_code ec;
	if (std::filesystem::exists(path, ec)) {
		return path;
	}
	if (ec)
		return path;

	std::filesystem::path newPath = ".";
	bool followingExisting = true;
	for (const auto &component : path) {
		std::filesystem::path newPath2 = newPath / component;
		if (followingExisting) {
			const bool exists = std::filesystem::exists(newPath2, ec);
			if (ec)
				return path;
			if (!exists && component != ".." && component != "." && component != "") {
				followingExisting = false;
				std::filesystem::directory_iterator iter{newPath, ec}, end;
				if (ec)
					return path;
				while (iter != end) {
					const auto &entry = iter->path();
					if (strcasecmp(entry.filename().c_str(), component.c_str()) == 0) {
						followingExisting = true;
						newPath2 = entry;
						break;
					}
					iter.increment(ec);
					if (ec)
						return path;
				}
			}
		}
		newPath = newPath2;
	}
	if (followingExisting) {
		DEBUG_LOG("Resolved case-insensitive path: %s\n", newPath.c_str());
	} else {
		DEBUG_LOG("Failed to resolve path: %s\n", newPath.c_str());
	}

	return newPath;
}

SystemSearchDirectories systemSearchDirectories() {
	const auto readDirectory = [](auto query) -> std::filesystem::path {
		std::vector<char> buffer(260);
		UINT length = query(buffer.data(), static_cast<UINT>(buffer.size()));
		if (length >= buffer.size()) {
			buffer.resize(length);
			length = query(buffer.data(), static_cast<UINT>(buffer.size()));
		}
		if (!length || length >= buffer.size())
			return {};
		return pathFromWindows(buffer.data());
	};
	SystemSearchDirectories directories;
	directories.system = readDirectory(kernel32::GetSystemDirectoryA);
	directories.windows = readDirectory(kernel32::GetWindowsDirectoryA);
	if (!directories.windows.empty())
		directories.legacySystem =
			findCaseInsensitiveFile(directories.windows, "System").value_or(directories.windows / "System");
	return directories;
}

std::string pathToWindows(const std::filesystem::path &path) {
	const std::filesystem::path normalized = path.lexically_normal();
	std::string str;
	if (normalized.is_absolute()) {
		// Keep round trips through GetFullPathName/GetCurrentDirectory on the
		// configured drive. Besides matching Windows drive semantics, this avoids
		// leaking (and repeatedly expanding) a potentially long host prefix into
		// compiler response and dependency data.
		if (const char *configuredRoot = std::getenv("WIBO_C_DRIVE"); configuredRoot && configuredRoot[0]) {
			std::error_code ec;
			std::filesystem::path root = std::filesystem::absolute(configuredRoot, ec).lexically_normal();
			if (!ec && normalized == root) {
				str = "C:/";
			} else if (!ec) {
				std::filesystem::path relative = normalized.lexically_relative(root);
				auto first = relative.begin();
				if (!relative.empty() && first != relative.end() && *first != "..") {
					str = "C:/" + relative.generic_string();
				}
			}
		}
		if (str.empty()) {
			str = "Z:" + normalized.generic_string();
		}
	} else {
		str = normalized.generic_string();
	}

	std::replace(str.begin(), str.end(), '/', '\\');
	return str;
}

static int snapshotStreamDescriptor(FileObject &file) {
	std::lock_guard lock(file.m);
	// A retained file object keeps its descriptor open after a handle is closed.
	return file.fd;
}

IOResult read(FileObject *file, void *buffer, size_t bytesToRead, const std::optional<off_t> &offset,
			  bool updateFilePointer) {
	IOResult result{};
	if (!file || !file->valid()) {
		result.unixError = EBADF;
		return result;
	}
	if (bytesToRead == 0) {
		return result;
	}

	// Sanity check: if no offset is given, we must update the file pointer
	assert(offset.has_value() || updateFilePointer);

	if (file->isPipe) {
		std::lock_guard streamLock(file->streamIoMutex);
		const int fd = snapshotStreamDescriptor(*file);
		size_t chunk = bytesToRead > SSIZE_MAX ? SSIZE_MAX : bytesToRead;
		uint8_t *in = static_cast<uint8_t *>(buffer);
		ssize_t rc;
		while (true) {
			rc = ::read(fd, in, chunk);
			if (rc == -1 && errno == EINTR) {
				continue;
			}
			break;
		}
		if (rc == -1) {
			result.unixError = errno ? errno : EIO;
			return result;
		}
		if (rc == 0) {
			result.reachedEnd = true;
			return result;
		}
		result.bytesTransferred = static_cast<size_t>(rc);
		return result;
	}

	std::lock_guard rangeGuard(file->m);
	CursorOperation operation(file->cursor);
	if (operation.error()) {
		result.unixError = operation.error();
		return result;
	}
	const auto doRead = [&](off_t pos) {
		result.windowsError = checkRangeAccess(file, pos, bytesToRead, false);
		if (result.windowsError) {
			return;
		}
		if (updateFilePointer && offset && lseek(file->fd, pos, SEEK_SET) == -1) {
			result.unixError = errno ? errno : EIO;
			return;
		}
		size_t total = 0;
		size_t remaining = bytesToRead;
		uint8_t *in = static_cast<uint8_t *>(buffer);
		while (remaining > 0) {
			size_t chunk = remaining > SSIZE_MAX ? SSIZE_MAX : remaining;
			ssize_t rc =
				updateFilePointer ? ::read(file->fd, in + total, chunk) : pread(file->fd, in + total, chunk, pos);
			if (rc == -1) {
				if (errno == EINTR) {
					continue;
				}
				result.unixError = errno ? errno : EIO;
				break;
			}
			if (rc == 0) {
				result.reachedEnd = true;
				break;
			}
			total += static_cast<size_t>(rc);
			remaining -= static_cast<size_t>(rc);
			pos += rc;
		}
		result.bytesTransferred = total;
	};

	if (offset) {
		doRead(*offset);
	} else {
		const off_t pos = lseek(file->fd, 0, SEEK_CUR);
		if (pos == -1)
			result.unixError = errno ? errno : EIO;
		else
			doRead(pos);
	}
	const int unlockError = operation.finish();
	if (!result.unixError && !result.windowsError)
		result.unixError = unlockError;

	return result;
}

IOResult write(FileObject *file, const void *buffer, size_t bytesToWrite, const std::optional<off_t> &offset,
			   bool updateFilePointer) {
	IOResult result{};
	if (!file || !file->valid()) {
		result.unixError = EBADF;
		return result;
	}
	if (bytesToWrite == 0) {
		return result;
	}

	// Sanity check: if no offset is given, we must update the file pointer
	assert(offset.has_value() || updateFilePointer);

	if (file->isPipe) {
		std::lock_guard streamLock(file->streamIoMutex);
		const int fd = snapshotStreamDescriptor(*file);
		size_t total = 0;
		size_t remaining = bytesToWrite;
		const uint8_t *in = static_cast<const uint8_t *>(buffer);
		while (remaining > 0) {
			size_t chunk = remaining > SSIZE_MAX ? SSIZE_MAX : remaining;
			ssize_t rc = ::write(fd, in + total, chunk);
			if (rc == -1) {
				if (errno == EINTR)
					continue;
				result.unixError = errno ? errno : EIO;
				break;
			}
			if (rc == 0)
				break;
			total += static_cast<size_t>(rc);
			remaining -= static_cast<size_t>(rc);
		}
		result.bytesTransferred = total;
		return result;
	}

	if (file->appendOnly) {
		std::lock_guard lk(file->m);
		CursorOperation operation(file->cursor);
		if (operation.error()) {
			result.unixError = operation.error();
			return result;
		}
		off_t originalPosition = 0;
		originalPosition = lseek(file->fd, 0, SEEK_CUR);
		if (originalPosition == -1) {
			result.unixError = errno ? errno : EIO;
			return result;
		}
		struct stat info{};
		if (fstat(file->fd, &info) != 0) {
			result.unixError = errno;
			return result;
		}
		result.windowsError = checkRangeAccess(file, info.st_size, bytesToWrite, true);
		if (result.windowsError) {
			return result;
		}
		size_t total = 0;
		size_t remaining = bytesToWrite;
		const uint8_t *in = static_cast<const uint8_t *>(buffer);
		while (remaining > 0) {
			size_t chunk = remaining > SSIZE_MAX ? SSIZE_MAX : remaining;
			ssize_t rc = ::write(file->fd, in + total, chunk);
			if (rc == -1) {
				if (errno == EINTR) {
					continue;
				}
				result.unixError = errno ? errno : EIO;
				break;
			}
			if (rc == 0) {
				break;
			}
			total += static_cast<size_t>(rc);
			remaining -= static_cast<size_t>(rc);
		}
		result.bytesTransferred = total;
		if (!updateFilePointer && lseek(file->fd, originalPosition, SEEK_SET) == -1 && result.unixError == 0) {
			result.unixError = errno ? errno : EIO;
		}
		const int unlockError = operation.finish();
		if (!result.unixError && !result.windowsError)
			result.unixError = unlockError;
		return result;
	}

	std::lock_guard rangeGuard(file->m);
	CursorOperation operation(file->cursor);
	if (operation.error()) {
		result.unixError = operation.error();
		return result;
	}
	auto doWrite = [&](off_t pos) {
		result.windowsError = checkRangeAccess(file, pos, bytesToWrite, true);
		if (result.windowsError) {
			return;
		}
		if (updateFilePointer && offset && lseek(file->fd, pos, SEEK_SET) == -1) {
			result.unixError = errno ? errno : EIO;
			return;
		}
		size_t total = 0;
		size_t remaining = bytesToWrite;
		const uint8_t *in = static_cast<const uint8_t *>(buffer);
		while (remaining > 0) {
			size_t chunk = remaining > SSIZE_MAX ? SSIZE_MAX : remaining;
			ssize_t rc =
				updateFilePointer ? ::write(file->fd, in + total, chunk) : pwrite(file->fd, in + total, chunk, pos);
			if (rc == -1) {
				if (errno == EINTR) {
					continue;
				}
				result.unixError = errno ? errno : EIO;
				break;
			}
			if (rc == 0) {
				break;
			}
			total += static_cast<size_t>(rc);
			remaining -= static_cast<size_t>(rc);
			pos += rc;
		}
		result.bytesTransferred = total;
	};

	if (offset) {
		doWrite(*offset);
	} else {
		const off_t pos = lseek(file->fd, 0, SEEK_CUR);
		if (pos == -1)
			result.unixError = errno ? errno : EIO;
		else
			doWrite(pos);
	}
	const int unlockError = operation.finish();
	if (!result.unixError && !result.windowsError)
		result.unixError = unlockError;

	return result;
}

HANDLE getStdHandle(DWORD nStdHandle) {
	switch (nStdHandle) {
	case STD_INPUT_HANDLE:
		return stdinHandle.load(std::memory_order_relaxed);
	case STD_OUTPUT_HANDLE:
		return stdoutHandle.load(std::memory_order_relaxed);
	case STD_ERROR_HANDLE:
		return stderrHandle.load(std::memory_order_relaxed);
	default:
		return INVALID_HANDLE_VALUE;
	}
}

BOOL setStdHandle(DWORD nStdHandle, HANDLE hHandle) {
	switch (nStdHandle) {
	case STD_INPUT_HANDLE:
		stdinHandle.store(hHandle, std::memory_order_relaxed);
		break;
	case STD_OUTPUT_HANDLE:
		stdoutHandle.store(hHandle, std::memory_order_relaxed);
		break;
	case STD_ERROR_HANDLE:
		stderrHandle.store(hHandle, std::memory_order_relaxed);
		break;
	default:
		return 0; // fail
	}
	return 1; // success
}

std::optional<StandardHandles> startupStandardHandles() {
	if (!explicitStartupStandards)
		return std::nullopt;
	return StandardHandles{getStdHandle(STD_INPUT_HANDLE), getStdHandle(STD_OUTPUT_HANDLE),
						   getStdHandle(STD_ERROR_HANDLE)};
}

void init(std::optional<StandardHandles> inheritedStandards) {
	signal(SIGPIPE, SIG_IGN);
	explicitStartupStandards = inheritedStandards && inheritedStandards->explicitStartup;
	if (inheritedStandards) {
		stdinHandle.store(inheritedStandards->input, std::memory_order_relaxed);
		stdoutHandle.store(inheritedStandards->output, std::memory_order_relaxed);
		stderrHandle.store(inheritedStandards->error, std::memory_order_relaxed);
		return;
	}
	auto &handles = wibo::handles();
	auto stdinObject = make_pin<FileObject>(STDIN_FILENO);
	stdinObject->closeOnDestroy = false;
	stdinHandle.store(handles.alloc(std::move(stdinObject), FILE_GENERIC_READ, HANDLE_FLAG_INHERIT),
					  std::memory_order_relaxed);
	auto stdoutObject = make_pin<FileObject>(STDOUT_FILENO);
	stdoutObject->closeOnDestroy = false;
	stdoutObject->appendOnly = true;
	stdoutHandle.store(handles.alloc(std::move(stdoutObject), FILE_GENERIC_WRITE, HANDLE_FLAG_INHERIT),
					   std::memory_order_relaxed);
	auto stderrObject = make_pin<FileObject>(STDERR_FILENO);
	stderrObject->closeOnDestroy = false;
	stderrObject->appendOnly = true;
	stderrHandle.store(handles.alloc(std::move(stderrObject), FILE_GENERIC_WRITE, HANDLE_FLAG_INHERIT),
					   std::memory_order_relaxed);
}

std::optional<std::filesystem::path> findCaseInsensitiveFile(const std::filesystem::path &directory,
															 const std::string &filename) {
	std::error_code ec;
	if (directory.empty()) {
		return std::nullopt;
	}
	if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec)) {
		return std::nullopt;
	}
	std::string needle = filename;
	toLowerInPlace(needle);
	for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
		if (ec) {
			break;
		}
		std::string candidate = entry.path().filename().string();
		toLowerInPlace(candidate);
		if (candidate == needle) {
			return canonicalPath(entry.path());
		}
	}
	auto direct = directory / filename;
	if (std::filesystem::exists(direct, ec)) {
		return canonicalPath(direct);
	}
	return std::nullopt;
}

std::filesystem::path canonicalPath(const std::filesystem::path &path) {
	std::error_code ec;
	auto canonical = std::filesystem::weakly_canonical(path, ec);
	if (!ec) {
		return canonical;
	}
	return std::filesystem::absolute(path);
}

std::string hostPathListToWindows(const std::string &value) {
	if (value.empty()) {
		return value;
	}
	char delimiter = value.find(';') != std::string::npos ? ';' : ':';
	auto entries = splitList(value, delimiter);
	std::string result;
	for (size_t i = 0; i < entries.size(); ++i) {
		if (i != 0) {
			result.push_back(';');
		}
		if (!entries[i].empty()) {
			result += toWindowsPathEntry(entries[i]);
		}
	}
	return result;
}

std::string windowsPathListToHost(const std::string &value) {
	if (value.empty()) {
		return value;
	}
	auto entries = splitList(value, ';');
	std::string result;
	for (size_t i = 0; i < entries.size(); ++i) {
		if (i != 0) {
			result.push_back(':');
		}
		if (!entries[i].empty()) {
			result += toHostPathEntry(entries[i]);
		}
	}
	return result;
}
} // namespace files
