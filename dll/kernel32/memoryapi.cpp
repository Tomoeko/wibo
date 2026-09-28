#include "memoryapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "heap.h"
#include "internal.h"
#include "modules.h"
#include "processthreadsapi.h"
#include "strutil.h"
#include "types.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif defined(__linux__)
#include <sys/uio.h>
#endif

namespace {

constexpr size_t kVirtualAllocationGranularity = 64 * 1024;
constexpr DWORD kSecImage = 0x01000000;
#ifdef WIBO_GUEST_64
constexpr uintptr_t kProcessAddressLimit = 0x0000800000000000ULL;
#else
constexpr uintptr_t kProcessAddressLimit = 0x80000000;
#endif

struct MappingObject : ObjectBase {
	static constexpr ObjectType kType = ObjectType::Mapping;

	std::mutex m;
	int fd = -1;
	size_t maxSize = 0;
	DWORD protect = 0;
	bool image = false;
	bool closed = false;

	explicit MappingObject() : ObjectBase(kType) {}
	~MappingObject() override;
};

MappingObject::~MappingObject() {
	if (fd != -1) {
		close(fd);
		fd = -1;
	}
}

struct ViewInfo {
	uintptr_t viewBase = 0;
	size_t viewLength = 0;
	uintptr_t allocationBase = 0;
	size_t allocationLength = 0;
	Pin<MappingObject> owner;
	std::unique_ptr<wibo::Executable> image;
	DWORD protect = PAGE_NOACCESS;
	DWORD allocationProtect = PAGE_NOACCESS;
	DWORD type = MEM_PRIVATE;
	bool managed = false;
};

std::map<uintptr_t, ViewInfo> g_viewInfo;
std::mutex g_viewInfoMutex;
std::unordered_set<uintptr_t> g_lockedPages;
std::mutex g_lockedPagesMutex;

uintptr_t alignDown(uintptr_t value, size_t alignment) {
	const uintptr_t mask = static_cast<uintptr_t>(alignment) - 1;
	return value & ~mask;
}

uintptr_t alignUp(uintptr_t value, size_t alignment) {
	const uintptr_t mask = static_cast<uintptr_t>(alignment) - 1;
	if (mask == std::numeric_limits<uintptr_t>::max()) {
		return value;
	}
	if (value > std::numeric_limits<uintptr_t>::max() - mask) {
		return std::numeric_limits<uintptr_t>::max();
	}
	return (value + mask) & ~mask;
}

struct FileMapAccess {
	bool read = false;
	bool write = false;
	bool execute = false;
	bool copy = false;
};

FileMapAccess fileMapAccessFromDesiredAccess(DWORD desiredAccess) {
	// FILE_MAP_ALL_ACCESS includes FILE_MAP_COPY's bit, but maps as a writable shared view.
	const bool allAccess = (desiredAccess & FILE_MAP_ALL_ACCESS) == FILE_MAP_ALL_ACCESS;
	FileMapAccess access{};
	access.read = allAccess || (desiredAccess & (FILE_MAP_READ | FILE_MAP_WRITE | FILE_MAP_COPY)) != 0;
	access.write = allAccess || (desiredAccess & FILE_MAP_WRITE) != 0;
	access.execute = (desiredAccess & FILE_MAP_EXECUTE) != 0;
	access.copy = !allAccess && (desiredAccess & FILE_MAP_COPY) != 0;
	if (access.copy) {
		access.write = true;
	}
	return access;
}

DWORD desiredAccessToProtect(DWORD desiredAccess, DWORD mappingProtect) {
	FileMapAccess access = fileMapAccessFromDesiredAccess(desiredAccess);
	const bool supportsWrite = mappingProtect == PAGE_READWRITE || mappingProtect == PAGE_EXECUTE_READWRITE ||
							   mappingProtect == PAGE_WRITECOPY || mappingProtect == PAGE_EXECUTE_WRITECOPY;
	const bool supportsCopy = mappingProtect == PAGE_WRITECOPY || mappingProtect == PAGE_EXECUTE_WRITECOPY;

	if (access.copy && !supportsCopy) {
		access.copy = false;
	}
	if (access.write && !supportsWrite) {
		if (supportsCopy) {
			access.copy = true;
			access.write = false;
		} else {
			access.write = false;
		}
	}
	if (!access.read && (mappingProtect == PAGE_READONLY || mappingProtect == PAGE_EXECUTE_READ ||
						 mappingProtect == PAGE_WRITECOPY || mappingProtect == PAGE_EXECUTE_WRITECOPY)) {
		access.read = true;
	}

	DWORD protect = PAGE_NOACCESS;
	if (access.copy && supportsCopy) {
		protect = access.execute ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY;
	} else if (access.execute) {
		if (access.write) {
			protect = PAGE_EXECUTE_READWRITE;
		} else if (access.read) {
			protect = PAGE_EXECUTE_READ;
		} else {
			protect = PAGE_EXECUTE;
		}
	} else {
		if (access.write) {
			protect = PAGE_READWRITE;
		} else if (access.read) {
			protect = PAGE_READONLY;
		}
	}
	if ((mappingProtect & PAGE_NOCACHE) != 0) {
		protect |= PAGE_NOCACHE;
	}
	if ((mappingProtect & PAGE_GUARD) != 0) {
		protect |= PAGE_GUARD;
	}
	if ((mappingProtect & PAGE_WRITECOMBINE) != 0) {
		protect |= PAGE_WRITECOMBINE;
	}
	return protect;
}

bool mappedViewRegionForAddress(uintptr_t request, uintptr_t pageBase, MEMORY_BASIC_INFORMATION &info) {
	std::lock_guard guard(g_viewInfoMutex);
	if (g_viewInfo.empty()) {
		return false;
	}
	const size_t pageSize = wibo::heap::systemPageSize();
	for (const auto &entry : g_viewInfo) {
		const ViewInfo &view = entry.second;
		if (view.viewLength == 0 || view.image) {
			continue;
		}
		uintptr_t allocationStart = view.allocationBase;
		uintptr_t allocationEnd = allocationStart + view.allocationLength;
		if (pageBase < allocationStart || pageBase >= allocationEnd) {
			continue;
		}
		uintptr_t viewStart = view.viewBase;
		uintptr_t viewEnd = view.viewBase + view.viewLength;
		if (request != 0 && (request < viewStart || request >= viewEnd)) {
			continue;
		}
		uintptr_t blockStart = pageBase;
		uintptr_t blockEnd = alignUp(viewEnd, pageSize);
		info.BaseAddress = toGuestPtr(reinterpret_cast<void *>(blockStart));
		info.AllocationBase = toGuestPtr(reinterpret_cast<void *>(view.viewBase));
		info.AllocationProtect = view.allocationProtect;
		info.RegionSize = blockEnd > blockStart ? blockEnd - blockStart : 0;
		info.State = MEM_COMMIT;
		info.Protect = view.protect;
		info.Type = view.type;
		return true;
	}
	return false;
}

bool protectAllowsFileGrowth(DWORD protect) { return protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE; }

bool fileSizeFromFd(int fd, uint64_t &size) {
	struct stat st{};
	if (fstat(fd, &st) != 0) {
		kernel32::setLastErrorFromErrno();
		return false;
	}
	if (st.st_size < 0) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return false;
	}
	size = static_cast<uint64_t>(st.st_size);
	return true;
}

DWORD fileGrowthErrorFromErrno(int err) {
	switch (err) {
	case ENOSPC:
#ifdef EDQUOT
	case EDQUOT:
#endif
#ifdef EFBIG
	case EFBIG:
#endif
		return ERROR_DISK_FULL;
	default:
		return wibo::winErrorFromErrno(err);
	}
}

bool growFileForMapping(int fd, uint64_t size) {
	if (size > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return false;
	}
	if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
		kernel32::setLastError(fileGrowthErrorFromErrno(errno));
		return false;
	}
	return true;
}

int createPagefileBacking(uint64_t size) {
	char path[] = "/tmp/wibo-mapping-XXXXXX";
	int fd = mkstemp(path);
	if (fd == -1) {
		kernel32::setLastErrorFromErrno();
		return -1;
	}
	unlink(path);
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
		kernel32::setLastErrorFromErrno();
		close(fd);
		return -1;
	}
	if (!growFileForMapping(fd, size)) {
		close(fd);
		return -1;
	}
	return fd;
}

std::u16string mappingName(LPCWSTR source) {
	std::u16string result;
	if (source) {
		for (; *source; ++source)
			result.push_back(static_cast<char16_t>(*source));
	}
	return result;
}

bool mappingHandleAllowsView(DWORD grantedAccess, DWORD desiredAccess) {
	const bool allAccess = (desiredAccess & FILE_MAP_ALL_ACCESS) == FILE_MAP_ALL_ACCESS;
	const bool wantsWrite = allAccess || (desiredAccess & FILE_MAP_WRITE) != 0;
	const bool wantsRead = !wantsWrite || (desiredAccess & (FILE_MAP_READ | FILE_MAP_COPY)) != 0;
	if (wantsWrite && (grantedAccess & FILE_MAP_WRITE) == 0)
		return false;
	if (wantsRead && (grantedAccess & (FILE_MAP_READ | FILE_MAP_WRITE)) == 0)
		return false;
	if ((desiredAccess & FILE_MAP_EXECUTE) && !(grantedAccess & FILE_MAP_EXECUTE))
		return false;
	return true;
}

bool mappingAccessForProtection(DWORD flProtect, DWORD &access) {
	const bool image = (flProtect & kSecImage) != 0;
	const DWORD protect = image ? flProtect & 0xFF : flProtect;
	const bool supportedImageProtect = protect == PAGE_READONLY || protect == PAGE_READWRITE ||
									   protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READ ||
									   protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
	if ((image && ((flProtect & ~0xFFu) != kSecImage || !supportedImageProtect)) ||
		(!image && protect != PAGE_READONLY && protect != PAGE_READWRITE && protect != PAGE_WRITECOPY)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return false;
	}
	access = FILE_MAP_READ;
	if (protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE)
		access |= FILE_MAP_WRITE;
	if (image)
		access |= FILE_MAP_EXECUTE;
	return true;
}

using MappingFile = std::unique_ptr<FILE, decltype(&fclose)>;

MappingFile openMappingFile(int fd) {
	int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (duplicate == -1) {
		kernel32::setLastErrorFromErrno();
		return {nullptr, &fclose};
	}
	FILE *file = fdopen(duplicate, "rb");
	if (!file) {
		int error = errno;
		close(duplicate);
		kernel32::setLastError(wibo::winErrorFromErrno(error));
	}
	return {file, &fclose};
}

} // namespace

namespace kernel32 {

static HANDLE createFileMappingUnnamed(HANDLE hFile, DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow,
									   DWORD handleFlags) {

	uint64_t size = (static_cast<uint64_t>(dwMaximumSizeHigh) << 32) | dwMaximumSizeLow;
	const bool image = (flProtect & kSecImage) != 0;
	const DWORD protect = image ? flProtect & 0xFF : flProtect;
	DWORD requestedAccess = 0;
	if (!mappingAccessForProtection(flProtect, requestedAccess)) {
		DEBUG_LOG("CreateFileMappingA: unsupported protection 0x%x\n", flProtect);
		return NO_HANDLE;
	}

	auto mapping = make_pin<MappingObject>();
	mapping->protect = protect;
	mapping->image = image;

	if (hFile == INVALID_HANDLE_VALUE) {
		if (image) {
			setLastError(ERROR_BAD_EXE_FORMAT);
			return NO_HANDLE;
		}
		if (size == 0) {
			setLastError(ERROR_INVALID_PARAMETER);
			return NO_HANDLE;
		}
		mapping->fd = createPagefileBacking(size);
		if (mapping->fd == -1)
			return NO_HANDLE;
		mapping->maxSize = size;
	} else {
		auto file = wibo::handles().getAs<FileObject>(hFile);
		if (!file || !file->valid()) {
			setLastError(ERROR_INVALID_HANDLE);
			return NO_HANDLE;
		}
		int dupFd = fcntl(file->fd, F_DUPFD_CLOEXEC, 0);
		if (dupFd == -1) {
			setLastErrorFromErrno();
			return NO_HANDLE;
		}
		mapping->fd = dupFd;
		if (image) {
			auto source = openMappingFile(dupFd);
			if (!source || !wibo::Executable::imageMappingSize(source.get(), mapping->maxSize)) {
				return NO_HANDLE;
			}
			if (size > mapping->maxSize) {
				setLastError(ERROR_NOT_ENOUGH_MEMORY);
				return NO_HANDLE;
			}
			return wibo::handles().alloc(std::move(mapping), requestedAccess, handleFlags);
		}
		uint64_t fileSize = 0;
		if (!fileSizeFromFd(dupFd, fileSize)) {
			return NO_HANDLE;
		}
		if (size == 0) {
			if (fileSize == 0) {
				setLastError(ERROR_FILE_INVALID);
				return NO_HANDLE;
			}
			size = fileSize;
		} else if (size > fileSize && protectAllowsFileGrowth(flProtect)) {
			if (!growFileForMapping(dupFd, size)) {
				return NO_HANDLE;
			}
			DEBUG_LOG("CreateFileMappingA: grew backing file from %llu to %llu bytes\n",
					  static_cast<unsigned long long>(fileSize), static_cast<unsigned long long>(size));
		}
		if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
			setLastError(ERROR_INVALID_PARAMETER);
			return NO_HANDLE;
		}
		mapping->maxSize = size;
	}

	return wibo::handles().alloc(std::move(mapping), requestedAccess, handleFlags);
}

HANDLE WINAPI CreateFileMappingW(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect,
								 DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCWSTR lpName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateFileMappingW(%p, %p, 0x%x, %u, %u, %p)\n", hFile, lpFileMappingAttributes, flProtect,
			  dwMaximumSizeHigh, dwMaximumSizeLow, lpName);
	if (lpFileMappingAttributes && lpFileMappingAttributes->lpSecurityDescriptor) {
		setLastError(ERROR_NOT_SUPPORTED);
		return NO_HANDLE;
	}
	const DWORD handleFlags =
		lpFileMappingAttributes && lpFileMappingAttributes->bInheritHandle ? HANDLE_FLAG_INHERIT : 0;
	DWORD requestedAccess = 0;
	if (!mappingAccessForProtection(flProtect, requestedAccess))
		return NO_HANDLE;
	const auto name = mappingName(lpName);
	if (!name.empty()) {
		if (auto object = wibo::g_namespace.get(name)) {
			auto existing = std::move(object).downcast<MappingObject>();
			if (!existing) {
				setLastError(ERROR_INVALID_HANDLE);
				return NO_HANDLE;
			}
			HANDLE handle = wibo::handles().alloc(std::move(existing), requestedAccess, handleFlags);
			setLastError(ERROR_ALREADY_EXISTS);
			return handle;
		}
	}
	HANDLE handle = createFileMappingUnnamed(hFile, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, handleFlags);
	if (!handle)
		return NO_HANDLE;
	if (!name.empty()) {
		auto created = wibo::handles().getAs<MappingObject>(handle);
		if (!wibo::g_namespace.insert(name, created.get())) {
			wibo::handles().release(handle);
			auto existing = wibo::g_namespace.getAs<MappingObject>(name);
			if (!existing) {
				setLastError(ERROR_INVALID_HANDLE);
				return NO_HANDLE;
			}
			handle = wibo::handles().alloc(std::move(existing), requestedAccess, handleFlags);
			setLastError(ERROR_ALREADY_EXISTS);
			return handle;
		}
	}
	setLastError(ERROR_SUCCESS);
	return handle;
}

HANDLE WINAPI CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect,
								 DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName) {
	HOST_CONTEXT_GUARD();
	std::vector<uint16_t> wideName;
	if (lpName)
		wideName = stringToWideString(lpName);
	return CreateFileMappingW(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow,
							  lpName ? wideName.data() : nullptr);
}

HANDLE WINAPI OpenFileMappingW(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCWSTR lpName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("OpenFileMappingW(0x%x, %d, %p)\n", dwDesiredAccess, bInheritHandle, lpName);
	if (!lpName) {
		setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}
	const auto name = mappingName(lpName);
	if (name.empty()) {
		setLastError(ERROR_INVALID_NAME);
		return NO_HANDLE;
	}
	constexpr DWORD allowedAccess = FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE;
	if (dwDesiredAccess & ~allowedAccess) {
		setLastError(ERROR_ACCESS_DENIED);
		return NO_HANDLE;
	}
	auto object = wibo::g_namespace.get(name);
	if (!object) {
		setLastError(ERROR_FILE_NOT_FOUND);
		return NO_HANDLE;
	}
	auto mapping = std::move(object).downcast<MappingObject>();
	if (!mapping) {
		setLastError(ERROR_INVALID_HANDLE);
		return NO_HANDLE;
	}
	const DWORD access = (dwDesiredAccess & FILE_MAP_COPY) ? (dwDesiredAccess | FILE_MAP_READ) : dwDesiredAccess;
	const DWORD flags = bInheritHandle ? HANDLE_FLAG_INHERIT : 0;
	return wibo::handles().alloc(std::move(mapping), access, flags);
}

HANDLE WINAPI OpenFileMappingA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName) {
	HOST_CONTEXT_GUARD();
	std::vector<uint16_t> wideName;
	if (lpName)
		wideName = stringToWideString(lpName);
	return OpenFileMappingW(dwDesiredAccess, bInheritHandle, lpName ? wideName.data() : nullptr);
}

bool lockRange(LPVOID address, SIZE_T size, uintptr_t &firstPage, uintptr_t &lastPage) {
	const uintptr_t start = reinterpret_cast<uintptr_t>(address);
	const size_t pageSize = wibo::heap::systemPageSize();
	if (!address || !size || size > std::numeric_limits<uintptr_t>::max() - start) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return false;
	}
	firstPage = alignDown(start, pageSize);
	lastPage = alignUp(start + size, pageSize);
	if (lastPage <= firstPage || lastPage == std::numeric_limits<uintptr_t>::max()) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return false;
	}
	return true;
}

static LPVOID mapViewOfFileInternal(Pin<MappingObject> mapping, DWORD dwDesiredAccess, uint64_t offset,
									SIZE_T dwNumberOfBytesToMap, LPVOID baseAddress) {
	if (!mapping) {
		setLastError(ERROR_INVALID_HANDLE);
		return nullptr;
	}
	if (mapping->closed) {
		setLastError(ERROR_INVALID_HANDLE);
		return nullptr;
	}
	if (mapping->image) {
		if (offset != 0) {
			setLastError(ERROR_INVALID_PARAMETER);
			return nullptr;
		}
		const FileMapAccess access = fileMapAccessFromDesiredAccess(dwDesiredAccess);
		if (access.write && !access.copy && !protectAllowsFileGrowth(mapping->protect)) {
			DEBUG_LOG("MapViewOfFile: writable image view access is not supported for this section\n");
			setLastError(ERROR_NOT_SUPPORTED);
			return nullptr;
		}
		auto source = openMappingFile(mapping->fd);
		if (!source) {
			return nullptr;
		}
		auto executable = std::make_unique<wibo::Executable>();
		if (!executable->mapImage(source.get(), baseAddress)) {
			return nullptr;
		}
		void *base = executable->imageBase;
		ViewInfo view{};
		view.viewBase = reinterpret_cast<uintptr_t>(base);
		view.viewLength = alignUp(executable->imageSize, wibo::heap::systemPageSize());
		view.allocationBase = view.viewBase;
		view.allocationLength = executable->imageSize;
		view.owner = std::move(mapping);
		view.image = std::move(executable);
		view.type = MEM_IMAGE;
		{
			std::lock_guard guard(g_viewInfoMutex);
			g_viewInfo.emplace(view.viewBase, std::move(view));
		}
		return base;
	}
	size_t maxSize = mapping->maxSize;
	uint64_t length = static_cast<uint64_t>(dwNumberOfBytesToMap);
	if (length == 0) {
		if (maxSize == 0 || offset > maxSize) {
			setLastError(ERROR_INVALID_PARAMETER);
			return nullptr;
		}
		length = maxSize - offset;
	}
	if (length == 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return nullptr;
	}
	if (maxSize != 0 && offset + length > maxSize) {
		setLastError(ERROR_INVALID_PARAMETER);
		return nullptr;
	}

	FileMapAccess access = fileMapAccessFromDesiredAccess(dwDesiredAccess);
	if (access.execute) {
		setLastError(ERROR_ACCESS_DENIED);
		return nullptr;
	}
	int prot = PROT_READ;
	if (mapping->protect == PAGE_READWRITE) {
		if (access.write || access.copy) {
			prot |= PROT_WRITE;
		}
	} else {
		if (access.write && !access.copy) {
			setLastError(ERROR_ACCESS_DENIED);
			return nullptr;
		}
		if (access.copy) {
			prot |= PROT_WRITE;
		}
	}
	if (access.execute) {
		prot |= PROT_EXEC;
	}

	int flags = access.copy ? MAP_PRIVATE : MAP_SHARED;
	const size_t pageSize = wibo::heap::systemPageSize();
	off_t alignedOffset = static_cast<off_t>(offset & ~static_cast<uint64_t>(pageSize - 1));
	size_t offsetDelta = static_cast<size_t>(offset - static_cast<uint64_t>(alignedOffset));
	uint64_t requestedLength = length + offsetDelta;
	if (requestedLength < length) {
		setLastError(ERROR_INVALID_PARAMETER);
		return nullptr;
	}
	size_t mapLength = static_cast<size_t>(requestedLength);
	if (static_cast<uint64_t>(mapLength) != requestedLength) {
		setLastError(ERROR_INVALID_PARAMETER);
		return nullptr;
	}

	int mmapFd = mapping->fd;
	void *requestedBase = nullptr;
	int mapFlags = flags;
	bool reservedMapping = false;
	if (baseAddress) {
		uintptr_t baseAddr = reinterpret_cast<uintptr_t>(baseAddress);
		if (baseAddr == 0 || (baseAddr % kVirtualAllocationGranularity) != 0) {
			setLastError(ERROR_INVALID_ADDRESS);
			return nullptr;
		}
		if (offsetDelta > baseAddr) {
			setLastError(ERROR_INVALID_ADDRESS);
			return nullptr;
		}
		uintptr_t mapBaseAddr = baseAddr - offsetDelta;
		if ((mapBaseAddr & (pageSize - 1)) != 0) {
			setLastError(ERROR_INVALID_ADDRESS);
			return nullptr;
		}
		requestedBase = reinterpret_cast<void *>(mapBaseAddr);
#ifdef MAP_FIXED_NOREPLACE
		mapFlags |= MAP_FIXED_NOREPLACE;
#else
		mapFlags |= MAP_FIXED;
#endif
	} else {
		void *candidate = nullptr;
		wibo::heap::VmStatus reserveStatus = wibo::heap::reserveViewRange(mapLength, 0, 0, &candidate);
		if (reserveStatus != wibo::heap::VmStatus::Success) {
			setLastError(wibo::heap::win32ErrorFromVmStatus(reserveStatus));
			return nullptr;
		}
		reservedMapping = true;
		requestedBase = candidate;
		mapFlags |= MAP_FIXED;
	}

	errno = 0;
	void *mapBase = mmap(requestedBase, mapLength, prot, mapFlags, mmapFd, alignedOffset);
	if (mapBase == MAP_FAILED) {
		int err = errno;
		if (baseAddress && (err == ENOMEM || err == EEXIST || err == EINVAL || err == EPERM)) {
			setLastError(ERROR_INVALID_ADDRESS);
		} else {
			setLastError(wibo::winErrorFromErrno(err));
		}
		if (reservedMapping) {
			wibo::heap::releaseViewRange(requestedBase);
		}
		return nullptr;
	}
	void *viewPtr = static_cast<uint8_t *>(mapBase) + offsetDelta;
	if (baseAddress && viewPtr != baseAddress) {
		munmap(mapBase, mapLength);
		setLastError(ERROR_INVALID_ADDRESS);
		if (reservedMapping) {
			wibo::heap::releaseViewRange(requestedBase);
		}
		return nullptr;
	}
	uintptr_t viewLength = static_cast<uintptr_t>(length);
	uintptr_t alignedViewLength = alignUp(viewLength, pageSize);
	if (alignedViewLength == std::numeric_limits<uintptr_t>::max()) {
		alignedViewLength = viewLength;
	}
	DWORD protect = mapping->protect;
	ViewInfo view{};
	view.viewBase = reinterpret_cast<uintptr_t>(viewPtr);
	view.viewLength = static_cast<size_t>(alignedViewLength);
	view.allocationBase = reinterpret_cast<uintptr_t>(mapBase);
	view.allocationLength = mapLength;
	view.owner = std::move(mapping);
	view.protect = desiredAccessToProtect(dwDesiredAccess, protect);
	view.allocationProtect = protect;
	view.type = MEM_MAPPED;
	view.managed = reservedMapping;
	if (reservedMapping) {
		wibo::heap::registerViewRange(mapBase, mapLength, protect, view.protect);
	}
	{
		std::lock_guard guard(g_viewInfoMutex);
		g_viewInfo.emplace(view.viewBase, std::move(view));
	}
	return viewPtr;
}

LPVOID WINAPI MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
							DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("MapViewOfFile(%p, 0x%x, %u, %u, %zu)\n", hFileMappingObject, dwDesiredAccess, dwFileOffsetHigh,
			  dwFileOffsetLow, dwNumberOfBytesToMap);

	HandleMeta meta{};
	auto mapping = wibo::handles().getAs<MappingObject>(hFileMappingObject, &meta);
	if (!mapping) {
		setLastError(ERROR_INVALID_HANDLE);
		return nullptr;
	}
	if (!mappingHandleAllowsView(meta.grantedAccess, dwDesiredAccess)) {
		setLastError(ERROR_ACCESS_DENIED);
		return nullptr;
	}
	uint64_t offset = (static_cast<uint64_t>(dwFileOffsetHigh) << 32) | dwFileOffsetLow;
	return mapViewOfFileInternal(std::move(mapping), dwDesiredAccess, offset, dwNumberOfBytesToMap, nullptr);
}

LPVOID WINAPI MapViewOfFileEx(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
							  DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap, LPVOID lpBaseAddress) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("MapViewOfFileEx(%p, 0x%x, %u, %u, %zu, %p)\n", hFileMappingObject, dwDesiredAccess, dwFileOffsetHigh,
			  dwFileOffsetLow, dwNumberOfBytesToMap, lpBaseAddress);

	HandleMeta meta{};
	auto mapping = wibo::handles().getAs<MappingObject>(hFileMappingObject, &meta);
	if (!mapping) {
		setLastError(ERROR_INVALID_HANDLE);
		return nullptr;
	}
	if (!mappingHandleAllowsView(meta.grantedAccess, dwDesiredAccess)) {
		setLastError(ERROR_ACCESS_DENIED);
		return nullptr;
	}
	uint64_t offset = (static_cast<uint64_t>(dwFileOffsetHigh) << 32) | dwFileOffsetLow;
	return mapViewOfFileInternal(std::move(mapping), dwDesiredAccess, offset, dwNumberOfBytesToMap, lpBaseAddress);
}

BOOL WINAPI UnmapViewOfFile(LPCVOID lpBaseAddress) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("UnmapViewOfFile(%p)\n", lpBaseAddress);
	std::unique_lock lk(g_viewInfoMutex);
	const uintptr_t address = reinterpret_cast<uintptr_t>(lpBaseAddress);
	auto it = g_viewInfo.upper_bound(address);
	if (it != g_viewInfo.begin()) {
		--it;
		const auto &view = it->second;
		if (address != view.viewBase && (!view.image || address - view.viewBase >= view.viewLength)) {
			it = g_viewInfo.end();
		}
	} else {
		it = g_viewInfo.end();
	}
	if (it == g_viewInfo.end()) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ViewInfo view = std::move(it->second);
	g_viewInfo.erase(it);
	lk.unlock();
	if (view.image) {
		return TRUE;
	}
	void *base = reinterpret_cast<void *>(view.allocationBase);
	size_t length = view.allocationLength;
	bool managed = view.managed;
	if (length != 0) {
		munmap(base, length);
	}
	if (managed) {
		wibo::heap::releaseViewRange(base);
	}
	return TRUE;
}

BOOL WINAPI FlushViewOfFile(LPCVOID lpBaseAddress, SIZE_T dwNumberOfBytesToFlush) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FlushViewOfFile(%p, %zu)\n", lpBaseAddress, dwNumberOfBytesToFlush);

	if (!lpBaseAddress) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	uintptr_t address = reinterpret_cast<uintptr_t>(lpBaseAddress);
	uintptr_t viewBase = 0;
	size_t viewLength = 0;
	uintptr_t allocationBase = 0;
	size_t allocationLength = 0;

	{
		std::lock_guard guard(g_viewInfoMutex);
		auto it = g_viewInfo.upper_bound(address);
		if (it == g_viewInfo.begin()) {
			setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		--it;
		const auto &view = it->second;
		if (address < view.viewBase || address >= view.viewBase + view.viewLength) {
			setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		viewBase = view.viewBase;
		viewLength = view.viewLength;
		allocationBase = view.allocationBase;
		allocationLength = view.allocationLength;
	}

	size_t offsetIntoView = static_cast<size_t>(address - viewBase);
	size_t bytesToFlush = dwNumberOfBytesToFlush;
	size_t maxFlush = viewLength - offsetIntoView;
	if (bytesToFlush == 0 || bytesToFlush > maxFlush) {
		bytesToFlush = maxFlush;
	}
	if (bytesToFlush == 0) {
		return TRUE;
	}

	uintptr_t flushStart = address;
	uintptr_t flushEnd = flushStart + bytesToFlush;
	const size_t pageSize = wibo::heap::systemPageSize();
	uintptr_t alignedStart = alignDown(flushStart, pageSize);
	uintptr_t alignedEnd = alignUp(flushEnd, pageSize);
	if (alignedEnd == std::numeric_limits<uintptr_t>::max()) {
		alignedEnd = flushEnd;
	}

	uintptr_t mappingEnd = allocationBase + allocationLength;
	if (alignedEnd > mappingEnd) {
		alignedEnd = mappingEnd;
	}
	if (alignedEnd < alignedStart) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	size_t length = static_cast<size_t>(alignedEnd - alignedStart);
	if (length == 0) {
		length = pageSize;
	}

	if (msync(reinterpret_cast<void *>(alignedStart), length, MS_SYNC) != 0) {
		setLastError(wibo::winErrorFromErrno(errno));
		return FALSE;
	}

	return TRUE;
}

BOOL WINAPI ReadProcessMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize,
							  PSIZE_T lpNumberOfBytesRead) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ReadProcessMemory(%p, %p, %p, %zu, %p)\n", hProcess, lpBaseAddress, lpBuffer, nSize,
			  lpNumberOfBytesRead);
	if (lpNumberOfBytesRead)
		*lpNumberOfBytesRead = 0;
	pid_t pid = getpid();
	if (!isPseudoCurrentProcessHandle(hProcess)) {
		HandleMeta metadata{};
		auto process = wibo::handles().getAs<ProcessObject>(hProcess, &metadata);
		if (!process) {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
		if (!(metadata.grantedAccess & PROCESS_VM_READ)) {
			setLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
		pid = process->pid;
	}
	if ((!lpBaseAddress || !lpBuffer) && nSize) {
		setLastError(ERROR_PARTIAL_COPY);
		return FALSE;
	}
	if (!nSize)
		return TRUE;

	SIZE_T copied = 0;
#if defined(__APPLE__)
	mach_port_t task = mach_task_self();
	if (pid != getpid() && task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	mach_vm_size_t transferred = 0;
	const kern_return_t status = mach_vm_read_overwrite(task, reinterpret_cast<mach_vm_address_t>(lpBaseAddress), nSize,
														reinterpret_cast<mach_vm_address_t>(lpBuffer), &transferred);
	if (pid != getpid())
		mach_port_deallocate(mach_task_self(), task);
	copied = static_cast<SIZE_T>(transferred);
	if (lpNumberOfBytesRead)
		*lpNumberOfBytesRead = copied;
	if (status != KERN_SUCCESS || copied != nSize) {
		setLastError(ERROR_PARTIAL_COPY);
		return FALSE;
	}
#elif defined(__linux__)
	iovec destination{lpBuffer, nSize};
	iovec source{const_cast<void *>(lpBaseAddress), nSize};
	const ssize_t transferred = process_vm_readv(pid, &destination, 1, &source, 1, 0);
	if (transferred >= 0)
		copied = static_cast<SIZE_T>(transferred);
	if (lpNumberOfBytesRead)
		*lpNumberOfBytesRead = copied;
	if (transferred < 0 || copied != nSize) {
		setLastError(transferred < 0 ? wibo::winErrorFromErrno(errno) : ERROR_PARTIAL_COPY);
		return FALSE;
	}
#else
	setLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
#endif
	return TRUE;
}

LPVOID WINAPI VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VirtualAlloc(%p, %zu, %u, %u)\n", lpAddress, dwSize, flAllocationType, flProtect);

	void *base = lpAddress;
	std::size_t size = static_cast<std::size_t>(dwSize);
	wibo::heap::VmStatus status = wibo::heap::virtualAlloc(&base, &size, flAllocationType, flProtect);
	if (status != wibo::heap::VmStatus::Success) {
		DWORD err = wibo::heap::win32ErrorFromVmStatus(status);
		DEBUG_LOG("-> failed (status=%u, err=%u)\n", static_cast<unsigned>(status), err);
		setLastError(err);
		return nullptr;
	}
	DEBUG_LOG("-> success (base=%p, size=%zu)\n", base, size);
	return base;
}

BOOL WINAPI VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VirtualFree(%p, %zu, %u)\n", lpAddress, dwSize, dwFreeType);
	MEMORY_BASIC_INFORMATION info{};
	if (wibo::heap::virtualQuery(lpAddress, &info) == wibo::heap::VmStatus::Success && info.Type == MEM_IMAGE) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	wibo::heap::VmStatus status = wibo::heap::virtualFree(lpAddress, static_cast<std::size_t>(dwSize), dwFreeType);
	if (status != wibo::heap::VmStatus::Success) {
		DWORD err = wibo::heap::win32ErrorFromVmStatus(status);
		DEBUG_LOG("-> failed (status=%u, err=%u)\n", static_cast<unsigned>(status), err);
		setLastError(err);
		return FALSE;
	}
	{
		std::lock_guard guard(g_lockedPagesMutex);
		for (auto it = g_lockedPages.begin(); it != g_lockedPages.end();) {
			MEMORY_BASIC_INFORMATION page{};
			if (wibo::heap::virtualQuery(reinterpret_cast<const void *>(*it), &page) != wibo::heap::VmStatus::Success ||
				page.State != MEM_COMMIT)
				it = g_lockedPages.erase(it);
			else
				++it;
		}
	}
	return TRUE;
}

BOOL WINAPI VirtualLock(LPVOID address, SIZE_T size) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VirtualLock(%p, %zu)\n", address, size);
	uintptr_t firstPage = 0, lastPage = 0;
	if (!lockRange(address, size, firstPage, lastPage))
		return FALSE;
	const size_t pageSize = wibo::heap::systemPageSize();
	if (mlock(reinterpret_cast<const void *>(firstPage), lastPage - firstPage) != 0) {
		setLastError(wibo::winErrorFromErrno(errno));
		return FALSE;
	}
	std::lock_guard guard(g_lockedPagesMutex);
	for (uintptr_t page = firstPage; page < lastPage; page += pageSize)
		g_lockedPages.insert(page);
	return TRUE;
}

BOOL WINAPI VirtualUnlock(LPVOID address, SIZE_T size) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VirtualUnlock(%p, %zu)\n", address, size);
	uintptr_t firstPage = 0, lastPage = 0;
	if (!lockRange(address, size, firstPage, lastPage))
		return FALSE;
	const size_t pageSize = wibo::heap::systemPageSize();
	std::lock_guard guard(g_lockedPagesMutex);
	bool missing = false;
	for (uintptr_t page = firstPage; page < lastPage; page += pageSize) {
		auto it = g_lockedPages.find(page);
		if (it == g_lockedPages.end()) {
			missing = true;
			continue;
		}
		if (munlock(reinterpret_cast<const void *>(page), pageSize) != 0) {
			setLastError(wibo::winErrorFromErrno(errno));
			return FALSE;
		}
		g_lockedPages.erase(it);
	}
	if (missing) {
		setLastError(ERROR_NOT_LOCKED);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VirtualProtect(%p, %zu, %u)\n", lpAddress, dwSize, flNewProtect);
	wibo::heap::VmStatus status =
		wibo::heap::virtualProtect(lpAddress, static_cast<std::size_t>(dwSize), flNewProtect, lpflOldProtect);
	if (status != wibo::heap::VmStatus::Success) {
		DWORD err = wibo::heap::win32ErrorFromVmStatus(status);
		DEBUG_LOG("-> failed (status=%u, err=%u)\n", static_cast<unsigned>(status), err);
		setLastError(err);
		return FALSE;
	}
	return TRUE;
}

SIZE_T WINAPI VirtualQuery(LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VirtualQuery(%p, %p, %zu)\n", lpAddress, lpBuffer, dwLength);
	if (!lpBuffer || dwLength < sizeof(MEMORY_BASIC_INFORMATION)) {
		setLastError(ERROR_INVALID_PARAMETER);
		DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
		return 0;
	}

	std::memset(lpBuffer, 0, sizeof(MEMORY_BASIC_INFORMATION));
	const size_t pageSize = wibo::heap::systemPageSize();
	uintptr_t request = lpAddress ? reinterpret_cast<uintptr_t>(lpAddress) : 0;
	uintptr_t pageBase = alignDown(request, pageSize);
	if (pageBase >= kProcessAddressLimit) {
		setLastError(ERROR_INVALID_PARAMETER);
		DEBUG_LOG("-> ERROR_INVALID_PARAMETER (beyond address space)\n");
		return 0;
	}

	MEMORY_BASIC_INFORMATION info{};
	if (mappedViewRegionForAddress(request, pageBase, info)) {
		*lpBuffer = info;
		return sizeof(MEMORY_BASIC_INFORMATION);
	}

	wibo::heap::VmStatus status = wibo::heap::virtualQuery(lpAddress, &info);
	if (status == wibo::heap::VmStatus::Success) {
		*lpBuffer = info;
		return sizeof(MEMORY_BASIC_INFORMATION);
	}

	DEBUG_LOG("VirtualQuery fallback failed status=%u\n", static_cast<unsigned>(status));
	setLastError(wibo::heap::win32ErrorFromVmStatus(status));
	DEBUG_LOG("-> VirtualQuery failed (status=%u)\n", static_cast<unsigned>(status));
	return 0;
}

BOOL WINAPI GetProcessWorkingSetSize(HANDLE hProcess, PSIZE_T lpMinimumWorkingSetSize,
									 PSIZE_T lpMaximumWorkingSetSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetProcessWorkingSetSize(%p, %p, %p)\n", hProcess, lpMinimumWorkingSetSize, lpMaximumWorkingSetSize);
	(void)hProcess;
	if (!lpMinimumWorkingSetSize || !lpMaximumWorkingSetSize) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*lpMinimumWorkingSetSize = 32 * 1024 * 1024;  // 32 MiB stub
	*lpMaximumWorkingSetSize = 128 * 1024 * 1024; // 128 MiB stub
	return TRUE;
}

BOOL WINAPI SetProcessWorkingSetSize(HANDLE hProcess, SIZE_T dwMinimumWorkingSetSize, SIZE_T dwMaximumWorkingSetSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetProcessWorkingSetSize(%p, %zu, %zu)\n", hProcess, dwMinimumWorkingSetSize, dwMaximumWorkingSetSize);
	(void)hProcess;
	(void)dwMinimumWorkingSetSize;
	(void)dwMaximumWorkingSetSize;
	return TRUE;
}

} // namespace kernel32
