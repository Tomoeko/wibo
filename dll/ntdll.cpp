#include "ntdll.h"

#include "common.h"
#include "context.h"
#include "context_x64.h"
#include "errors.h"
#include "files.h"
#include "handles.h"
#include "heap.h"
#include "kernel32/fileapi.h"
#include "kernel32/internal.h"
#include "kernel32/minwinbase.h"
#include "kernel32/processthreadsapi.h"
#include "kernel32/synchapi.h"
#include "modules.h"
#include "processes.h"
#include "strutil.h"
#include "types.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

#include <optional>

namespace {

struct PROCESS_BASIC_INFORMATION {
	NTSTATUS ExitStatus;
	GUEST_PTR PebBaseAddress;
	ULONG_PTR AffinityMask;
	LONG BasePriority;
	ULONG_PTR UniqueProcessId;
	ULONG_PTR InheritedFromUniqueProcessId;
};

struct ProcessHandleDetails {
	pid_t pid = -1;
	DWORD exitCode = STILL_ACTIVE;
	PEB *peb = nullptr;
	bool isCurrentProcess = false;
};

constexpr LONG kDefaultBasePriority = 8;

struct RTL_OSVERSIONINFOEXW : RTL_OSVERSIONINFOW {
	WORD wServicePackMajor;
	WORD wServicePackMinor;
	WORD wSuiteMask;
	BYTE wProductType;
	BYTE wReserved;
};

using PRTL_OSVERSIONINFOEXW = RTL_OSVERSIONINFOEXW *;

constexpr ULONG kOsMajorVersion = 6;
constexpr ULONG kOsMinorVersion = 2;
constexpr ULONG kOsBuildNumber = 0;
constexpr ULONG kOsPlatformId = 2;			// VER_PLATFORM_WIN32_NT
constexpr BYTE kProductTypeWorkstation = 1; // VER_NT_WORKSTATION

constexpr ULONGLONG kHundredNanosecondsPerSecond = 10'000'000ULL;
constexpr ULONGLONG kUnixEpochAsFileTime = 116'444'736'000'000'000ULL;

struct StatFetchResult {
	bool ok = false;
	int err = 0;
};

#if defined(__APPLE__)
timespec accessTimespec(const struct stat &st) { return st.st_atimespec; }
timespec modifyTimespec(const struct stat &st) { return st.st_mtimespec; }
timespec changeTimespec(const struct stat &st) { return st.st_ctimespec; }
#elif defined(__linux__)
timespec accessTimespec(const struct stat &st) { return st.st_atim; }
timespec modifyTimespec(const struct stat &st) { return st.st_mtim; }
timespec changeTimespec(const struct stat &st) { return st.st_ctim; }
#else
timespec accessTimespec(const struct stat &st) { return timespec{.tv_sec = st.st_atime, .tv_nsec = 0}; }
timespec modifyTimespec(const struct stat &st) { return timespec{.tv_sec = st.st_mtime, .tv_nsec = 0}; }
timespec changeTimespec(const struct stat &st) { return timespec{.tv_sec = st.st_ctime, .tv_nsec = 0}; }
#endif

LONGLONG timespecToFileTime(const timespec &ts) {
#if defined(__SIZEOF_INT128__)
	__int128 ticks = static_cast<__int128>(ts.tv_sec) * static_cast<__int128>(kHundredNanosecondsPerSecond);
	ticks += static_cast<__int128>(ts.tv_nsec / 100);
	ticks += static_cast<__int128>(kUnixEpochAsFileTime);
	if (ticks < 0) {
		return 0;
	}
	if (ticks > static_cast<__int128>(std::numeric_limits<int64_t>::max())) {
		return std::numeric_limits<int64_t>::max();
	}
	return static_cast<LONGLONG>(ticks);
#else
	long double ticks = static_cast<long double>(ts.tv_sec) * static_cast<long double>(kHundredNanosecondsPerSecond);
	ticks += static_cast<long double>(ts.tv_nsec) / 100.0L;
	ticks += static_cast<long double>(kUnixEpochAsFileTime);
	if (ticks < 0.0L) {
		return 0;
	}
	if (ticks > static_cast<long double>(std::numeric_limits<long long>::max())) {
		return std::numeric_limits<long long>::max();
	}
	return static_cast<LONGLONG>(ticks);
#endif
}

DWORD buildFileAttributes(const struct stat &st) {
	DWORD attributes = 0;
	mode_t mode = st.st_mode;
	if (S_ISDIR(mode)) {
		attributes |= FILE_ATTRIBUTE_DIRECTORY;
	}
	if (S_ISREG(mode)) {
		attributes |= FILE_ATTRIBUTE_ARCHIVE;
	}
	if ((mode & S_IWUSR) == 0) {
		attributes |= FILE_ATTRIBUTE_READONLY;
	}
	if (attributes == 0) {
		attributes = FILE_ATTRIBUTE_NORMAL;
	}
	return attributes;
}

StatFetchResult fetchStat(kernel32::FsObject *fs, struct stat &st) {
	if (!fs) {
		return {};
	}
	if (fs->valid()) {
		if (fstat(fs->fd, &st) == 0) {
			return StatFetchResult{.ok = true, .err = 0};
		}
		if (errno != EBADF) {
			return StatFetchResult{.ok = false, .err = errno};
		}
	}
	if (!fs->canonicalPath.empty()) {
		if (stat(fs->canonicalPath.c_str(), &st) == 0) {
			return StatFetchResult{.ok = true, .err = 0};
		}
		return StatFetchResult{.ok = false, .err = errno};
	}
	return StatFetchResult{};
}

void populateBasicInformation(const struct stat &st, FILE_BASIC_INFORMATION &info) {
	info = {};
#ifdef __APPLE__
	info.CreationTime.QuadPart = timespecToFileTime(st.st_birthtimespec);
#else
	info.CreationTime.QuadPart = timespecToFileTime(changeTimespec(st));
#endif
	info.LastAccessTime.QuadPart = timespecToFileTime(accessTimespec(st));
	info.LastWriteTime.QuadPart = timespecToFileTime(modifyTimespec(st));
	info.ChangeTime.QuadPart = timespecToFileTime(changeTimespec(st));
	info.FileAttributes = buildFileAttributes(st);
}

void populateStandardInformation(const kernel32::FsObject &file, const struct stat &st,
								 FILE_STANDARD_INFORMATION &info) {
	info = {};
	const bool directory = S_ISDIR(st.st_mode);
	info.AllocationSize.QuadPart = directory ? 0 : static_cast<LONGLONG>(st.st_blocks) * 512;
	info.EndOfFile.QuadPart = directory ? 0 : static_cast<LONGLONG>(st.st_size);
	info.NumberOfLinks = static_cast<ULONG>(st.st_nlink);
	info.DeletePending = file.deletePending ? TRUE : FALSE;
	info.Directory = directory ? TRUE : FALSE;
}

std::vector<WCHAR> fileInformationName(const kernel32::FsObject &file) {
	std::string path = file.canonicalPath.empty() ? "" : files::pathToWindows(file.canonicalPath);
	if (path.size() >= 2 && path[1] == ':')
		path.erase(0, 2);
	if (!path.empty() && path.front() != '\\')
		path.insert(path.begin(), '\\');
	auto wide = stringToWideString(path.c_str(), path.size());
	if (!wide.empty())
		wide.pop_back(); // Native file names are byte-counted and exclude a terminator.
	return wide;
}

bool directoryNameMatches(std::u16string_view pattern, std::u16string_view name) {
	size_t patternIndex = 0, nameIndex = 0;
	size_t star = std::u16string_view::npos, retry = 0;
	auto upper = [](char16_t c) { return c >= u'a' && c <= u'z' ? c - (u'a' - u'A') : c; };
	while (nameIndex < name.size()) {
		if (patternIndex < pattern.size() &&
			(pattern[patternIndex] == u'?' || upper(pattern[patternIndex]) == upper(name[nameIndex]))) {
			++patternIndex;
			++nameIndex;
		} else if (patternIndex < pattern.size() && pattern[patternIndex] == u'*') {
			star = patternIndex++;
			retry = nameIndex;
		} else if (star != std::u16string_view::npos) {
			patternIndex = star + 1;
			nameIndex = ++retry;
		} else {
			return false;
		}
	}
	while (patternIndex < pattern.size() && pattern[patternIndex] == u'*')
		++patternIndex;
	return patternIndex == pattern.size();
}

NTSTATUS readDirectoryEntries(kernel32::DirectoryObject &directory) {
	// A separate open description keeps native enumeration independent of duplicate handles.
	int fd = openat(directory.fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return wibo::statusFromErrno(errno);
	DIR *stream = fdopendir(fd);
	if (!stream) {
		int error = errno;
		close(fd);
		return wibo::statusFromErrno(error);
	}
	std::vector<std::string> entries;
	int error = 0;
	for (;;) {
		errno = 0;
		dirent *entry = readdir(stream);
		if (!entry) {
			error = errno;
			break;
		}
		entries.emplace_back(entry->d_name);
	}
	closedir(stream);
	if (error)
		return wibo::statusFromErrno(error);
	directory.enumEntries = std::move(entries);
	directory.enumCookie = 0;
	return STATUS_SUCCESS;
}

bool resolveProcessDetails(HANDLE processHandle, ProcessHandleDetails &details) {
	if (kernel32::isPseudoCurrentProcessHandle(processHandle)) {
		details.pid = getpid();
		details.exitCode = STILL_ACTIVE;
		details.peb = wibo::processPeb;
		details.isCurrentProcess = true;
		return true;
	}

	auto po = wibo::handles().getAs<ProcessObject>(processHandle);
	if (!po) {
		return false;
	}

	details.pid = po->pid;
	details.exitCode = po->exitCode;
	details.isCurrentProcess = po->pid == getpid();
	details.peb = details.isCurrentProcess ? wibo::processPeb : nullptr;
	return true;
}

std::string windowsImagePathFor(const ProcessHandleDetails &details) {
	if (details.isCurrentProcess && !wibo::guestExecutablePath.empty()) {
		return files::pathToWindows(files::canonicalPath(wibo::guestExecutablePath));
	}

	std::error_code ec;
	std::filesystem::path link = std::filesystem::path("/proc") / std::to_string(details.pid) / "exe";
	std::filesystem::path resolved = std::filesystem::read_symlink(link, ec);
	if (!ec) {
		return files::pathToWindows(files::canonicalPath(resolved));
	}
	return {};
}

} // namespace

namespace kernel32 {

NTSTATUS queryStandardInformationLocked(FsObject &file, FILE_STANDARD_INFORMATION &information) {
	struct stat st{};
	const auto fetched = fetchStat(&file, st);
	if (!fetched.ok)
		return wibo::statusFromErrno(fetched.err ? fetched.err : EINVAL);
	populateStandardInformation(file, st, information);
	return STATUS_SUCCESS;
}

} // namespace kernel32

namespace ntdll {

constexpr LARGE_INTEGER FILE_WRITE_TO_END_OF_FILE = {.QuadPart = -1};
constexpr LARGE_INTEGER FILE_USE_FILE_POINTER_POSITION = {.QuadPart = -2};

PVOID CDECL memset(PVOID dest, int ch, SIZE_T count) {
	VERBOSE_LOG("ntdll::memset(%p, %i, %zu)\n", dest, ch, count);
	return std::memset(dest, ch, count);
}

BYTE CDECL __wine_dbg_get_channel_flags(WINE_DEBUG_CHANNEL *channel) {
	if (channel) {
		channel->flags = 0;
	}
	return 0;
}

int CDECL __wine_dbg_header(WINE_DEBUG_CLASS debugClass, WINE_DEBUG_CHANNEL *channel, const char *function) {
	(void)debugClass;
	(void)function;
	if (channel) {
		channel->flags = 0;
	}
	return -1;
}

int CDECL __wine_dbg_output(const char *str) {
	return str ? static_cast<int>(std::strlen(str)) : 0;
}

const char *CDECL __wine_dbg_strdup(const char *str) {
	if (!str) {
		return nullptr;
	}

	constexpr size_t kBufferCount = 32;
	constexpr size_t kBufferSize = 1024;
	thread_local char buffers[kBufferCount][kBufferSize];
	thread_local size_t nextBuffer = 0;
	char *buffer = buffers[nextBuffer++ % kBufferCount];
	std::strncpy(buffer, str, kBufferSize - 1);
	buffer[kBufferSize - 1] = '\0';
	return buffer;
}

NTSTATUS WINAPI NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
						   PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset,
						   PULONG Key) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtReadFile(%p, %p, %p, %p, %p, %p, %u, %p, %p) ", FileHandle, Event, ApcRoutine, ApcContext,
			  IoStatusBlock, Buffer, Length, ByteOffset, Key);
	(void)ApcRoutine;
	(void)ApcContext;
	(void)Key;

	if (!IoStatusBlock) {
		return STATUS_INVALID_PARAMETER;
	}
	IoStatusBlock->Information = 0;

	auto file = wibo::handles().getAs<FileObject>(FileHandle);
	if (!file || !file->valid()) {
		IoStatusBlock->Status = STATUS_INVALID_HANDLE;
		IoStatusBlock->Information = 0;
		return STATUS_INVALID_HANDLE;
	}

	bool useOverlapped = file->overlapped;
	bool useCurrentFilePosition = (ByteOffset == nullptr);
	if (!useCurrentFilePosition && ByteOffset->QuadPart == FILE_USE_FILE_POINTER_POSITION.QuadPart) {
		useCurrentFilePosition = true;
	}

	std::optional<off_t> offset;
	if (!useCurrentFilePosition) {
		offset = static_cast<off_t>(ByteOffset->QuadPart);
	}

	if (useOverlapped && useCurrentFilePosition) {
		IoStatusBlock->Status = STATUS_INVALID_PARAMETER;
		IoStatusBlock->Information = 0;
		return STATUS_INVALID_PARAMETER;
	}

	Pin<kernel32::EventObject> ev;
	if (Event) {
		ev = wibo::handles().getAs<kernel32::EventObject>(Event);
		if (!ev) {
			IoStatusBlock->Status = STATUS_INVALID_HANDLE;
			IoStatusBlock->Information = 0;
			return STATUS_INVALID_HANDLE;
		}
		ev->reset();
	}

	bool updateFilePointer = !useOverlapped;
	auto io = files::read(file.get(), Buffer, Length, offset, updateFilePointer);
	NTSTATUS status = STATUS_SUCCESS;
	if (io.windowsError != 0) {
		status = wibo::statusFromWinError(io.windowsError);
	} else if (io.unixError != 0) {
		status = wibo::statusFromErrno(io.unixError);
	} else if (io.reachedEnd && io.bytesTransferred == 0) {
		status = file->isPipe ? STATUS_PIPE_BROKEN : STATUS_END_OF_FILE;
	}

	IoStatusBlock->Status = status;
	IoStatusBlock->Information = static_cast<ULONG_PTR>(io.bytesTransferred);

	if (ev && (status == STATUS_SUCCESS || status == STATUS_END_OF_FILE)) {
		ev->set();
	}

	DEBUG_LOG("-> 0x%x\n", status);
	return status;
}

NTSTATUS WINAPI NtWriteFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
							PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset,
							PULONG Key) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtWriteFile(%p, %p, %p, %p, %p, %p, %u, %p, %p) ", FileHandle, Event, ApcRoutine, ApcContext,
			  IoStatusBlock, Buffer, Length, ByteOffset, Key);
	(void)ApcRoutine;
	(void)ApcContext;
	(void)Key;

	if (!IoStatusBlock) {
		return STATUS_INVALID_PARAMETER;
	}
	IoStatusBlock->Information = 0;

	auto file = wibo::handles().getAs<FileObject>(FileHandle);
	if (!file || !file->valid()) {
		IoStatusBlock->Status = STATUS_INVALID_HANDLE;
		return STATUS_INVALID_HANDLE;
	}

	bool useOverlapped = file->overlapped;
	bool useCurrentFilePosition = (ByteOffset == nullptr);
	bool writeToEndOfFile = false;
	if (ByteOffset) {
		if (ByteOffset->QuadPart == FILE_USE_FILE_POINTER_POSITION.QuadPart) {
			useCurrentFilePosition = true;
		} else if (ByteOffset->QuadPart == FILE_WRITE_TO_END_OF_FILE.QuadPart) {
			writeToEndOfFile = true;
		}
	}

	std::optional<off_t> offset;
	if (!useCurrentFilePosition && !writeToEndOfFile) {
		offset = static_cast<off_t>(ByteOffset->QuadPart);
	}

	if (useOverlapped && useCurrentFilePosition) {
		IoStatusBlock->Status = STATUS_INVALID_PARAMETER;
		return STATUS_INVALID_PARAMETER;
	}

	Pin<kernel32::EventObject> ev;
	if (Event) {
		ev = wibo::handles().getAs<kernel32::EventObject>(Event);
		if (!ev) {
			IoStatusBlock->Status = STATUS_INVALID_HANDLE;
			return STATUS_INVALID_HANDLE;
		}
		ev->reset();
	}

	bool updateFilePointer = file->isPipe ? true : !useOverlapped;

	if (writeToEndOfFile && !offset.has_value()) {
		if (!file->isPipe) {
			struct stat st{};
			if (fstat(file->fd, &st) != 0) {
				int err = errno ? errno : EIO;
				NTSTATUS status = wibo::statusFromErrno(err);
				IoStatusBlock->Status = status;
				return status;
			}
			offset = static_cast<off_t>(st.st_size);
		}
	}

	auto io = files::write(file.get(), Buffer, static_cast<size_t>(Length), offset, updateFilePointer);
	NTSTATUS status = STATUS_SUCCESS;
	if (io.windowsError != 0) {
		status = wibo::statusFromWinError(io.windowsError);
	} else if (io.unixError != 0) {
		status = wibo::statusFromErrno(io.unixError);
	}

	IoStatusBlock->Status = status;
	IoStatusBlock->Information = static_cast<ULONG_PTR>(io.bytesTransferred);

	if (ev && status == STATUS_SUCCESS) {
		ev->set();
	}

	DEBUG_LOG("-> 0x%x\n", status);
	return status;
}

NTSTATUS WINAPI NtAllocateVirtualMemory(HANDLE ProcessHandle, guest_ptr<> *BaseAddress, ULONG_PTR ZeroBits,
										PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtAllocateVirtualMemory(%p, %p, %lu, %p, %lu, %lu) ", ProcessHandle, BaseAddress, ZeroBits, RegionSize,
			  AllocationType, Protect);
	if (ProcessHandle != (HANDLE)-1) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_HANDLE);
		return STATUS_INVALID_HANDLE;
	}
	if (ZeroBits != 0 || BaseAddress == nullptr || RegionSize == nullptr) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_PARAMETER);
		return STATUS_INVALID_PARAMETER;
	}

	void *baseAddress = BaseAddress->get();
	size_t regionSize = static_cast<size_t>(*RegionSize);
	wibo::heap::VmStatus vmStatus = wibo::heap::virtualAlloc(
		&baseAddress, &regionSize, static_cast<DWORD>(AllocationType), static_cast<DWORD>(Protect));
	if (vmStatus != wibo::heap::VmStatus::Success) {
		NTSTATUS status = wibo::heap::ntStatusFromVmStatus(vmStatus);
		DEBUG_LOG("-> 0x%x\n", status);
		return status;
	}

	*BaseAddress = baseAddress;
	*RegionSize = static_cast<SIZE_T>(regionSize);

	DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
	return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtProtectVirtualMemory(HANDLE ProcessHandle, guest_ptr<> *BaseAddress, PSIZE_T NumberOfBytesToProtect,
									   ULONG NewAccessProtection, PULONG OldAccessProtection) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtProtectVirtualMemory(%p, %p, %p, %lu, %p) ", ProcessHandle, BaseAddress, NumberOfBytesToProtect,
			  NewAccessProtection, OldAccessProtection);
	if (ProcessHandle != (HANDLE)-1) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_HANDLE);
		return STATUS_INVALID_HANDLE;
	}
	if (BaseAddress == nullptr || NumberOfBytesToProtect == nullptr) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_PARAMETER);
		return STATUS_INVALID_PARAMETER;
	}

	void *base = BaseAddress->get();
	size_t length = static_cast<size_t>(*NumberOfBytesToProtect);
	wibo::heap::VmStatus vmStatus =
		wibo::heap::virtualProtect(base, length, static_cast<DWORD>(NewAccessProtection), OldAccessProtection);
	if (vmStatus != wibo::heap::VmStatus::Success) {
		NTSTATUS status = wibo::heap::ntStatusFromVmStatus(vmStatus);
		DEBUG_LOG("-> 0x%x\n", status);
		return status;
	}

	DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
	return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtQueryDirectoryFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apcRoutine, PVOID apcContext,
									 PIO_STATUS_BLOCK ioStatus, PVOID information, ULONG length,
									 FILE_INFORMATION_CLASS informationClass, BOOLEAN singleEntry,
									 UNICODE_STRING *fileName, BOOLEAN restartScan) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtQueryDirectoryFile(%p, %p, %p, %p, %p, %p, %u, %u, %u, %p, %u)\n", file, event, apcRoutine, apcContext,
			  ioStatus, information, length, static_cast<unsigned>(informationClass), singleEntry, fileName,
			  restartScan);
	static_assert(offsetof(FILE_DIRECTORY_INFORMATION, FileName) == 64);
	static_assert(sizeof(FILE_DIRECTORY_INFORMATION) == 72);
	if (!ioStatus || !information)
		return STATUS_ACCESS_VIOLATION;
	if (informationClass != FileDirectoryInformation && informationClass != FileFullDirectoryInformation)
		return STATUS_NOT_SUPPORTED;
	if (length < sizeof(FILE_DIRECTORY_INFORMATION))
		return STATUS_INFO_LENGTH_MISMATCH;
	// Async completion and APC delivery must not be silently omitted.
	if (event || apcRoutine || apcContext)
		return STATUS_NOT_SUPPORTED;
	HandleMeta metadata{};
	auto object = wibo::handles().getAs<kernel32::FsObject>(file, &metadata);
	if (!object || !object->valid())
		return STATUS_INVALID_HANDLE;
	if (object->type != ObjectType::Directory)
		return STATUS_INVALID_PARAMETER;
	if (!(metadata.grantedAccess & FILE_LIST_DIRECTORY))
		return STATUS_ACCESS_DENIED;
	if (object->openFlags & FILE_FLAG_OVERLAPPED)
		return STATUS_NOT_SUPPORTED;
	auto directoryHandle = std::move(object).downcast<kernel32::DirectoryObject>();
	std::lock_guard lock(directoryHandle->m);
	auto &directory = *directoryHandle;
	bool first = !directory.enumStarted;
	if (first) {
		std::u16string pattern = u"*";
		if (fileName) {
			if (fileName->Length % sizeof(WCHAR) || fileName->Length > fileName->MaximumLength)
				return STATUS_INVALID_PARAMETER;
			if (fileName->Length && !fileName->Buffer)
				return STATUS_ACCESS_VIOLATION;
			const auto *buffer = reinterpret_cast<const char16_t *>(fileName->Buffer);
			if (fileName->Length)
				pattern.assign(buffer, fileName->Length / sizeof(WCHAR));
		}
		// Extended DOS expressions and non-ASCII case folding need separate support.
		for (char16_t c : pattern) {
			if (c >= 0x80 || c == u'<' || c == u'>' || c == u'"')
				return STATUS_NOT_SUPPORTED;
			if (c == 0 || c == u'/' || c == u'\\')
				return STATUS_OBJECT_NAME_INVALID;
		}
		directory.enumPattern = std::move(pattern);
	}
	ioStatus->Information = 0;
	auto finish = [&](NTSTATUS status) {
		ioStatus->Status = status;
		DEBUG_LOG("-> 0x%x, bytes=%llu\n", status, static_cast<unsigned long long>(ioStatus->Information));
		return status;
	};
	if (first || restartScan) {
		NTSTATUS status = readDirectoryEntries(directory);
		if (status != STATUS_SUCCESS)
			return finish(status);
		directory.enumStarted = true;
	}
	static_assert(offsetof(FILE_FULL_DIRECTORY_INFORMATION, FileName) == 68);
	static_assert(sizeof(FILE_FULL_DIRECTORY_INFORMATION) == 72);
	const size_t prefix = informationClass == FileFullDirectoryInformation
							  ? offsetof(FILE_FULL_DIRECTORY_INFORMATION, FileName)
							  : offsetof(FILE_DIRECTORY_INFORMATION, FileName);
	size_t written = 0, previous = 0;
	while (directory.enumCookie < directory.enumEntries.size()) {
		const auto &name = directory.enumEntries[directory.enumCookie];
		auto wide = utf8ToUtf16(name);
		if (!wide)
			return finish(STATUS_OBJECT_NAME_INVALID);
		if (!directoryNameMatches(directory.enumPattern, *wide)) {
			++directory.enumCookie;
			continue;
		}
		struct stat st{};
		if (fstatat(directory.fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
			if (errno == ENOENT) {
				++directory.enumCookie;
				continue; // The entry disappeared during enumeration.
			}
			return finish(wibo::statusFromErrno(errno));
		}
		bool symlink = S_ISLNK(st.st_mode);
		if (symlink && fstatat(directory.fd, name.c_str(), &st, 0) != 0)
			return finish(wibo::statusFromErrno(errno));
		size_t offset = (written + 7) & ~size_t(7);
		size_t nameBytes = wide->size() * sizeof(WCHAR);
		size_t recordBytes = prefix + nameBytes;
		bool overflow = offset + recordBytes > length;
		if (overflow && (written || !first))
			break;
		FILE_BASIC_INFORMATION basic{};
		populateBasicInformation(st, basic);
		FILE_FULL_DIRECTORY_INFORMATION entry{};
		entry.CreationTime = basic.CreationTime;
		entry.LastAccessTime = basic.LastAccessTime;
		entry.LastWriteTime = basic.LastWriteTime;
		entry.ChangeTime = basic.ChangeTime;
		entry.EndOfFile.QuadPart = S_ISDIR(st.st_mode) ? 0 : st.st_size;
		entry.AllocationSize.QuadPart = S_ISDIR(st.st_mode) ? 0 : static_cast<LONGLONG>(st.st_blocks) * 512;
		entry.FileAttributes = basic.FileAttributes | (symlink ? FILE_ATTRIBUTE_REPARSE_POINT : 0);
		entry.FileNameLength = static_cast<ULONG>(nameBytes);
		auto *output = static_cast<BYTE *>(information);
		std::memset(output + written, 0, offset - written);
		std::memcpy(output + offset, &entry, prefix);
		std::memcpy(output + offset + prefix, wide->data(), std::min(nameBytes, length - offset - prefix));
		if (written) {
			ULONG next = static_cast<ULONG>(offset - previous);
			std::memcpy(output + previous, &next, sizeof(next));
		}
		previous = offset;
		written = overflow ? length : offset + recordBytes;
		ioStatus->Information = static_cast<ULONG_PTR>(written);
		++directory.enumCookie;
		if (overflow)
			return finish(STATUS_BUFFER_OVERFLOW);
		if (singleEntry)
			break;
	}
	if (written || directory.enumCookie < directory.enumEntries.size())
		return finish(STATUS_SUCCESS);
	return finish(first ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES);
}

NTSTATUS WINAPI NtQueryInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
									   ULONG Length, FILE_INFORMATION_CLASS FileInformationClass) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtQueryInformationFile(%p, %p, %p, %u, %u) ", FileHandle, IoStatusBlock, FileInformation, Length,
			  static_cast<unsigned>(FileInformationClass));

	if (!IoStatusBlock) {
		DEBUG_LOG("-> 0x%x\n", STATUS_ACCESS_VIOLATION);
		return STATUS_ACCESS_VIOLATION;
	}

	IoStatusBlock->Information = 0;

	if (Length != 0 && !FileInformation) {
		IoStatusBlock->Status = STATUS_ACCESS_VIOLATION;
		DEBUG_LOG("-> 0x%x\n", STATUS_ACCESS_VIOLATION);
		return STATUS_ACCESS_VIOLATION;
	}

	if (static_cast<LONG_PTR>(FileHandle) < 0) {
		IoStatusBlock->Status = STATUS_OBJECT_TYPE_MISMATCH;
		DEBUG_LOG("-> 0x%x\n", STATUS_OBJECT_TYPE_MISMATCH);
		return STATUS_OBJECT_TYPE_MISMATCH;
	}

	HandleMeta metadata{};
	auto obj = wibo::handles().getAs<kernel32::FsObject>(FileHandle, &metadata);
	if (!obj || !obj->valid()) {
		IoStatusBlock->Status = STATUS_INVALID_HANDLE;
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_HANDLE);
		return STATUS_INVALID_HANDLE;
	}

	std::lock_guard lock(obj->m);
	NTSTATUS status = STATUS_SUCCESS;

	switch (FileInformationClass) {
	case FileBasicInformation: {
		if (Length < sizeof(FILE_BASIC_INFORMATION)) {
			status = STATUS_INFO_LENGTH_MISMATCH;
			break;
		}
		struct stat st{};
		StatFetchResult statRes = fetchStat(obj.get(), st);
		if (!statRes.ok) {
			status = wibo::statusFromErrno(statRes.err != 0 ? statRes.err : EINVAL);
			break;
		}
		auto info = reinterpret_cast<PFILE_BASIC_INFORMATION>(FileInformation);
		populateBasicInformation(st, *info);
		IoStatusBlock->Information = sizeof(FILE_BASIC_INFORMATION);
		break;
	}
	case FileStandardInformation: {
		if (Length < sizeof(FILE_STANDARD_INFORMATION)) {
			status = STATUS_INFO_LENGTH_MISMATCH;
			break;
		}
		auto info = reinterpret_cast<PFILE_STANDARD_INFORMATION>(FileInformation);
		status = kernel32::queryStandardInformationLocked(*obj, *info);
		if (status == STATUS_SUCCESS)
			IoStatusBlock->Information = sizeof(FILE_STANDARD_INFORMATION);
		break;
	}
	case FilePositionInformation: {
		auto file = std::move(obj).downcast<kernel32::FileObject>();
		if (!file) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}
		if (Length < sizeof(FILE_POSITION_INFORMATION)) {
			status = STATUS_INFO_LENGTH_MISMATCH;
			break;
		}
		auto info = reinterpret_cast<PFILE_POSITION_INFORMATION>(FileInformation);
		info->CurrentByteOffset.QuadPart = static_cast<LONGLONG>(file->filePos);
		IoStatusBlock->Information = sizeof(FILE_POSITION_INFORMATION);
		break;
	}
	case FileAllInformation: {
		static_assert(offsetof(FILE_ALL_INFORMATION, NameInformation.FileName) == 100);
		if (Length < sizeof(FILE_ALL_INFORMATION)) {
			status = STATUS_INFO_LENGTH_MISMATCH;
			break;
		}
		// The mapped backend has byte alignment and does not implement uncached I/O.
		if (obj->openFlags & FILE_FLAG_NO_BUFFERING) {
			status = STATUS_NOT_SUPPORTED;
			break;
		}
		struct stat st{};
		const auto fetched = fetchStat(obj.get(), st);
		if (!fetched.ok) {
			status = wibo::statusFromErrno(fetched.err ? fetched.err : EINVAL);
			break;
		}
		FILE_ALL_INFORMATION info{};
		populateBasicInformation(st, info.BasicInformation);
		populateStandardInformation(*obj, st, info.StandardInformation);
		info.IndexNumber.QuadPart = static_cast<LONGLONG>(st.st_ino);
		// Extended attributes are not represented by this mapped filesystem.
		info.EaSize = 0;
		info.AccessFlags = metadata.grantedAccess;
		if (obj->flags & Of_File) {
			auto file = obj.clone().downcast<kernel32::FileObject>();
			info.PositionInformation.CurrentByteOffset.QuadPart = file->filePos;
		}
		info.Mode = (obj->openFlags & FILE_FLAG_OVERLAPPED) ? 0 : 0x20; // FILE_SYNCHRONOUS_IO_NONALERT
		if (obj->openFlags & FILE_FLAG_WRITE_THROUGH)
			info.Mode |= 0x2;
		if (obj->openFlags & 0x08000000) // FILE_FLAG_SEQUENTIAL_SCAN
			info.Mode |= 0x4;
		if (obj->deletePending)
			info.Mode |= 0x1000;
		info.AlignmentRequirement = 0; // FILE_BYTE_ALIGNMENT
		const auto name = fileInformationName(*obj);
		const size_t required = name.size() * sizeof(WCHAR);
		constexpr size_t prefix = offsetof(FILE_ALL_INFORMATION, NameInformation.FileName);
		const size_t copied = std::min(required, static_cast<size_t>(Length) - prefix);
		info.NameInformation.FileNameLength = static_cast<ULONG>(required);
		std::memcpy(FileInformation, &info, prefix);
		if (copied)
			std::memcpy(static_cast<BYTE *>(FileInformation) + prefix, name.data(), copied);
		IoStatusBlock->Information = prefix + copied;
		if (copied < required)
			status = static_cast<NTSTATUS>(0x80000005); // STATUS_BUFFER_OVERFLOW
		break;
	}
	case FileNameInformation: {
		if (Length < sizeof(ULONG)) {
			status = STATUS_INFO_LENGTH_MISMATCH;
			break;
		}
		auto info = reinterpret_cast<PFILE_NAME_INFORMATION>(FileInformation);
		const auto wide = fileInformationName(*obj);
		size_t bytesRequired = wide.size() * sizeof(WCHAR);
		if (Length < sizeof(ULONG) + bytesRequired) {
			info->FileNameLength = static_cast<ULONG>(bytesRequired);
			status = STATUS_INFO_LENGTH_MISMATCH;
			break;
		}
		info->FileNameLength = static_cast<ULONG>(bytesRequired);
		if (bytesRequired > 0) {
			std::memcpy(info->FileName, wide.data(), bytesRequired);
		}
		IoStatusBlock->Information = static_cast<ULONG>(sizeof(ULONG) + bytesRequired);
		break;
	}
	default:
		DEBUG_LOG("FIXME: NtQueryInformationFile: Unsupported info class");
		status = STATUS_INVALID_INFO_CLASS;
		break;
	}

	IoStatusBlock->Status = status;
	DEBUG_LOG("-> 0x%x\n", status);
	return status;
}

NTSTATUS WINAPI NtQuerySystemTime(PLARGE_INTEGER SystemTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtQuerySystemTime(%p) ", SystemTime);
	if (!SystemTime) {
		DEBUG_LOG("-> 0x%x\n", STATUS_ACCESS_VIOLATION);
		return STATUS_ACCESS_VIOLATION;
	}

	using HundredNanoseconds = std::chrono::duration<long long, std::ratio<1, 10000000>>;
	auto now = std::chrono::system_clock::now().time_since_epoch();
	auto sinceUnix = std::chrono::duration_cast<HundredNanoseconds>(now).count();
	ULONGLONG fileTime = kUnixEpochAsFileTime + static_cast<ULONGLONG>(sinceUnix);
	SystemTime->QuadPart = static_cast<LONGLONG>(fileTime);

	DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
	return STATUS_SUCCESS;
}

BOOLEAN WINAPI RtlTimeToSecondsSince1970(PLARGE_INTEGER Time, PULONG ElapsedSeconds) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlTimeToSecondsSince1970(%p, %p) ", Time, ElapsedSeconds);
	if (!Time || !ElapsedSeconds) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	LONGLONG fileTimeSigned = Time->QuadPart;
	if (fileTimeSigned < 0) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	ULONGLONG fileTime = static_cast<ULONGLONG>(fileTimeSigned);
	if (fileTime < kUnixEpochAsFileTime) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	ULONGLONG delta = fileTime - kUnixEpochAsFileTime;
	ULONGLONG seconds = delta / kHundredNanosecondsPerSecond;
	if (seconds > 0xFFFFFFFFULL) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	*ElapsedSeconds = static_cast<ULONG>(seconds);
	DEBUG_LOG("-> %u\n", TRUE);
	return TRUE;
}

VOID WINAPI RtlInitializeBitMap(PRTL_BITMAP BitMapHeader, PULONG BitMapBuffer, ULONG SizeOfBitMap) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlInitializeBitMap(%p, %p, %u)\n", BitMapHeader, BitMapBuffer, SizeOfBitMap);
	if (!BitMapHeader) {
		return;
	}

	BitMapHeader->SizeOfBitMap = SizeOfBitMap;
	BitMapHeader->Buffer = BitMapBuffer;
}

VOID WINAPI RtlSetBits(PRTL_BITMAP BitMapHeader, ULONG StartingIndex, ULONG NumberToSet) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlSetBits(%p, %u, %u)\n", BitMapHeader, StartingIndex, NumberToSet);
	if (!BitMapHeader || !BitMapHeader->Buffer || NumberToSet == 0) {
		return;
	}

	ULONG size = BitMapHeader->SizeOfBitMap;
	if (StartingIndex >= size) {
		return;
	}

	ULONG available = size - StartingIndex;
	if (NumberToSet > available) {
		NumberToSet = available;
	}

	for (ULONG i = 0; i < NumberToSet; ++i) {
		ULONG bitIndex = StartingIndex + i;
		ULONG wordIndex = bitIndex / 32;
		ULONG offset = bitIndex % 32;
		BitMapHeader->Buffer[wordIndex] |= (1u << offset);
	}
}

BOOLEAN WINAPI RtlAreBitsSet(PRTL_BITMAP BitMapHeader, ULONG StartingIndex, ULONG Length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlAreBitsSet(%p, %u, %u) ", BitMapHeader, StartingIndex, Length);
	if (!BitMapHeader || !BitMapHeader->Buffer || Length == 0) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	ULONG size = BitMapHeader->SizeOfBitMap;
	if (StartingIndex >= size || Length > size - StartingIndex) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	for (ULONG i = 0; i < Length; ++i) {
		ULONG bitIndex = StartingIndex + i;
		ULONG wordIndex = bitIndex / 32;
		ULONG offset = bitIndex % 32;
		if ((BitMapHeader->Buffer[wordIndex] & (1u << offset)) == 0) {
			DEBUG_LOG("-> %u\n", FALSE);
			return FALSE;
		}
	}

	DEBUG_LOG("-> %u\n", TRUE);
	return TRUE;
}

BOOLEAN WINAPI RtlAreBitsClear(PRTL_BITMAP BitMapHeader, ULONG StartingIndex, ULONG Length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlAreBitsClear(%p, %u, %u) ", BitMapHeader, StartingIndex, Length);
	if (!BitMapHeader || !BitMapHeader->Buffer || Length == 0) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	ULONG size = BitMapHeader->SizeOfBitMap;
	if (StartingIndex >= size || Length > size - StartingIndex) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	for (ULONG i = 0; i < Length; ++i) {
		ULONG bitIndex = StartingIndex + i;
		ULONG wordIndex = bitIndex / 32;
		ULONG offset = bitIndex % 32;
		if ((BitMapHeader->Buffer[wordIndex] & (1u << offset)) != 0) {
			DEBUG_LOG("-> %u\n", FALSE);
			return FALSE;
		}
	}

	DEBUG_LOG("-> %u\n", TRUE);
	return TRUE;
}

BOOL WINAPI RtlIsCriticalSectionLockedByThread(RTL_CRITICAL_SECTION *CriticalSection) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlIsCriticalSectionLockedByThread(%p)\n", CriticalSection);
	if (!CriticalSection) {
		return FALSE;
	}
	const HANDLE currentThread = static_cast<HANDLE>(kernel32::GetCurrentThreadId());
	const HANDLE owner = __atomic_load_n(&CriticalSection->OwningThread, __ATOMIC_ACQUIRE);
	// Read the owner-only recursion field only when this thread owns the lock.
	// Ownership publication uses the same acquire/release pair as kernel32.
	return owner == currentThread && CriticalSection->RecursionCount != 0;
}

ULONGLONG WINAPI VerSetConditionMask(ULONGLONG ConditionMask, DWORD TypeMask, BYTE Condition) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VerSetConditionMask(0x%llx, 0x%x, %u)\n", ConditionMask, TypeMask, Condition);
	// Each recognized version field has a three-bit operator. Existing bits
	// accumulate; one call selects only the highest recognized field bit.
	for (unsigned int bit = 8; bit != 0; --bit) {
		if (TypeMask & (1u << (bit - 1))) {
			return ConditionMask | (static_cast<ULONGLONG>(Condition & 7) << (3 * (bit - 1)));
		}
	}
	return ConditionMask;
}

NTSTATUS WINAPI RtlGetVersion(PRTL_OSVERSIONINFOW lpVersionInformation) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlGetVersion(%p) ", lpVersionInformation);
	if (!lpVersionInformation) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_PARAMETER);
		return STATUS_INVALID_PARAMETER;
	}

	ULONG size = lpVersionInformation->dwOSVersionInfoSize;
	if (size < sizeof(RTL_OSVERSIONINFOW)) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_PARAMETER);
		return STATUS_INVALID_PARAMETER;
	}

	std::memset(lpVersionInformation, 0, static_cast<size_t>(size));
	lpVersionInformation->dwOSVersionInfoSize = size;
	lpVersionInformation->dwMajorVersion = kOsMajorVersion;
	lpVersionInformation->dwMinorVersion = kOsMinorVersion;
	lpVersionInformation->dwBuildNumber = kOsBuildNumber;
	lpVersionInformation->dwPlatformId = kOsPlatformId;

	if (size >= sizeof(RTL_OSVERSIONINFOEXW)) {
		auto extended = reinterpret_cast<PRTL_OSVERSIONINFOEXW>(lpVersionInformation);
		extended->wProductType = kProductTypeWorkstation;
	}

	DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
	return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtQueryInformationProcess(HANDLE ProcessHandle, PROCESSINFOCLASS ProcessInformationClass,
										  PVOID ProcessInformation, ULONG ProcessInformationLength,
										  PULONG ReturnLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtQueryInformationProcess(%d, %u, %p, %u, %p) ", ProcessHandle, ProcessInformationClass,
			  ProcessInformation, ProcessInformationLength, ReturnLength);
	if (!ProcessInformation) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_PARAMETER);
		return STATUS_INVALID_PARAMETER;
	}

	ProcessHandleDetails details{};
	if (!resolveProcessDetails(ProcessHandle, details)) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_HANDLE);
		return STATUS_INVALID_HANDLE;
	}

	switch (ProcessInformationClass) {
	case ProcessBasicInformation: {
		size_t required = sizeof(PROCESS_BASIC_INFORMATION);
		if (ReturnLength) {
			*ReturnLength = static_cast<ULONG>(required);
		}
		if (ProcessInformationLength < required) {
			DEBUG_LOG("-> 0x%x\n", STATUS_INFO_LENGTH_MISMATCH);
			return STATUS_INFO_LENGTH_MISMATCH;
		}

		auto *info = reinterpret_cast<PROCESS_BASIC_INFORMATION *>(ProcessInformation);
		std::memset(info, 0, sizeof(*info));
		info->ExitStatus = static_cast<NTSTATUS>(details.exitCode);
		info->PebBaseAddress = toGuestPtr(details.peb);
		DWORD_PTR processMask = 0;
		DWORD_PTR systemMask = 0;
		if (kernel32::GetProcessAffinityMask(ProcessHandle, &processMask, &systemMask)) {
			info->AffinityMask = static_cast<ULONG_PTR>(processMask == 0 ? 1 : processMask);
		} else {
			info->AffinityMask = 1;
		}
		info->BasePriority = kDefaultBasePriority;
		info->UniqueProcessId = static_cast<ULONG_PTR>(details.pid);
		if (details.isCurrentProcess) {
			info->InheritedFromUniqueProcessId = static_cast<ULONG_PTR>(getppid());
		} else {
			info->InheritedFromUniqueProcessId = static_cast<ULONG_PTR>(getpid());
		}
		DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
		return STATUS_SUCCESS;
	}
	case ProcessWow64Information: {
		size_t required = sizeof(ULONG_PTR);
		if (ReturnLength) {
			*ReturnLength = static_cast<ULONG>(required);
		}
		if (ProcessInformationLength < required) {
			DEBUG_LOG("-> 0x%x\n", STATUS_INFO_LENGTH_MISMATCH);
			return STATUS_INFO_LENGTH_MISMATCH;
		}
		auto *value = reinterpret_cast<ULONG_PTR *>(ProcessInformation);
		*value = 0;
		DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
		return STATUS_SUCCESS;
	}
	case ProcessImageFileName: {
		size_t minimum = sizeof(UNICODE_STRING);
		if (ProcessInformationLength < minimum) {
			if (ReturnLength) {
				*ReturnLength = static_cast<ULONG>(minimum);
			}
			DEBUG_LOG("-> 0x%x\n", STATUS_INFO_LENGTH_MISMATCH);
			return STATUS_INFO_LENGTH_MISMATCH;
		}

		std::string imagePath = windowsImagePathFor(details);
		DEBUG_LOG("  NtQueryInformationProcess image path: %s\n", imagePath.c_str());
		auto widePath = stringToWideString(imagePath.c_str());
		size_t stringBytes = widePath.size() * sizeof(uint16_t);
		size_t required = sizeof(UNICODE_STRING) + stringBytes;
		if (ReturnLength) {
			*ReturnLength = static_cast<ULONG>(required);
		}
		if (ProcessInformationLength < required) {
			DEBUG_LOG("-> 0x%x\n", STATUS_INFO_LENGTH_MISMATCH);
			return STATUS_INFO_LENGTH_MISMATCH;
		}

		auto *unicode = reinterpret_cast<UNICODE_STRING *>(ProcessInformation);
		auto *buffer =
			reinterpret_cast<uint16_t *>(reinterpret_cast<uint8_t *>(ProcessInformation) + sizeof(UNICODE_STRING));
		std::memcpy(buffer, widePath.data(), stringBytes);
		size_t characterCount = widePath.empty() ? 0 : widePath.size() - 1;
		unicode->Length = static_cast<unsigned short>(characterCount * sizeof(uint16_t));
		unicode->MaximumLength = static_cast<unsigned short>(widePath.size() * sizeof(uint16_t));
		unicode->Buffer = toGuestPtr(buffer);
		DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
		return STATUS_SUCCESS;
	}
	default:
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_INFO_CLASS);
		return STATUS_INVALID_INFO_CLASS;
	}
}

NTSTATUS WINAPI LdrAddRefDll(ULONG Flags, HMODULE Module) {
	DEBUG_LOG("STUB: LdrAddRefDll(%x, %p)\n", Flags, Module);
	(void)Flags;
	(void)Module;
	return STATUS_SUCCESS;
}

} // namespace ntdll

#include "ntdll_trampolines.h"

extern const wibo::ModuleStub lib_ntdll = {
	(const char *[]){
		"ntdll",
		nullptr,
	},
	ntdllThunkByName,
	nullptr,
};
