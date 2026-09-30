#include "sysinfoapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "ntdll.h"
#include "system_provider.h"
#include "timeutil.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <sys/time.h>

namespace {

#ifdef WIBO_GUEST_64
constexpr WORD kSystemProcessorArchitecture = 9; // PROCESSOR_ARCHITECTURE_AMD64
constexpr DWORD kSystemProcessorType = 8664;	 // PROCESSOR_AMD_X8664
#else
constexpr WORD kSystemProcessorArchitecture = 0; // PROCESSOR_ARCHITECTURE_INTEL
constexpr DWORD kSystemProcessorType = 586;		 // PROCESSOR_INTEL_PENTIUM
#endif

constexpr uint64_t kUnixTimeZero = 11644473600ULL * 10000000ULL;
constexpr DWORD kMajorVersion = 6;
constexpr DWORD kMinorVersion = 2;
constexpr DWORD kBuildNumber = 0;

bool queryCurrentFileTime(FILETIME &result) {
#if defined(CLOCK_REALTIME)
	struct timespec ts{};
	if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
		uint64_t ticks = kUnixTimeZero;
		ticks += static_cast<uint64_t>(ts.tv_sec) * 10000000ULL;
		ticks += static_cast<uint64_t>(ts.tv_nsec) / 100ULL;
		result = fileTimeFromDuration(ticks);
		return true;
	}
#endif
	struct timeval tv{};
	if (gettimeofday(&tv, nullptr) == 0) {
		uint64_t ticks = kUnixTimeZero;
		ticks += static_cast<uint64_t>(tv.tv_sec) * 10000000ULL;
		ticks += static_cast<uint64_t>(tv.tv_usec) * 10ULL;
		result = fileTimeFromDuration(ticks);
		return true;
	}
	return false;
}

void writeCurrentFileTime(LPFILETIME output) {
	if (!output) {
		return;
	}
	const DWORD previousError = kernel32::getLastError();
	FILETIME result{};
	if (!queryCurrentFileTime(result)) {
		std::perror("Unable to read system time");
		std::_Exit(ERROR_NOT_SUPPORTED);
	}
	std::memcpy(output, &result, sizeof(result));
	kernel32::setLastError(previousError);
}

DWORD_PTR computeSystemProcessorMask(unsigned int cpuCount) {
	const auto maskWidth = static_cast<unsigned int>(sizeof(DWORD_PTR) * 8);
	if (cpuCount >= maskWidth) {
		return static_cast<DWORD_PTR>(~static_cast<DWORD_PTR>(0));
	}
	DWORD_PTR mask = (static_cast<DWORD_PTR>(1) << cpuCount) - 1;
	return mask == 0 ? 1 : mask;
}

constexpr DWORD ERROR_BAD_ARGUMENTS = 160;
constexpr DWORD ERROR_OLD_WIN_VERSION = 1150;

BYTE versionCondition(ULONGLONG mask, unsigned int fieldBit) { return static_cast<BYTE>((mask >> (3 * fieldBit)) & 7); }

bool matchesVersionValue(DWORD actual, DWORD requested, BYTE condition) {
	switch (condition) {
	case VER_EQUAL:
		return actual == requested;
	case VER_GREATER:
		return actual > requested;
	case VER_GREATER_EQUAL:
		return actual >= requested;
	case VER_LESS:
		return actual < requested;
	case VER_LESS_EQUAL:
		return actual <= requested;
	default:
		return false;
	}
}

bool isVersionRelation(BYTE condition) { return condition >= VER_EQUAL && condition <= VER_LESS_EQUAL; }

bool sameVersionDirection(BYTE governing, BYTE next) {
	if (next == VER_EQUAL) {
		return isVersionRelation(governing);
	}
	const bool increasing = governing == VER_GREATER || governing == VER_GREATER_EQUAL;
	const bool decreasing = governing == VER_LESS || governing == VER_LESS_EQUAL;
	return (increasing && (next == VER_GREATER || next == VER_GREATER_EQUAL)) ||
		   (decreasing && (next == VER_LESS || next == VER_LESS_EQUAL));
}

template <typename VersionInfo>
DWORD verifyVersionConditions(const VersionInfo *requested, DWORD fields, ULONGLONG mask) {
	if (!requested) {
		return ERROR_INVALID_PARAMETER;
	}
	if (!fields || !mask) {
		return ERROR_BAD_ARGUMENTS;
	}

	// Use the same version facade as GetVersionExA/W, including its product and
	// service-pack values. This does not introduce manifest-based version lies.
	OSVERSIONINFOEXW actual{};
	actual.dwOSVersionInfoSize = sizeof(actual);
	const NTSTATUS status = ntdll::RtlGetVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(&actual));
	if (status != STATUS_SUCCESS) {
		return wibo::winErrorFromNtStatus(status);
	}

	if ((fields & VER_PRODUCT_TYPE) &&
		!matchesVersionValue(actual.wProductType, requested->wProductType, versionCondition(mask, 7))) {
		return ERROR_OLD_WIN_VERSION;
	}
	if (fields & VER_SUITENAME) {
		const WORD present = actual.wSuiteMask & requested->wSuiteMask;
		switch (versionCondition(mask, 6)) {
		case VER_AND:
			if (present != requested->wSuiteMask) {
				return ERROR_OLD_WIN_VERSION;
			}
			break;
		case VER_OR:
			// An empty requested suite imposes no requirement for either mode.
			if (requested->wSuiteMask && !present) {
				return ERROR_OLD_WIN_VERSION;
			}
			break;
		default:
			return ERROR_BAD_ARGUMENTS;
		}
	}
	if ((fields & VER_PLATFORMID) &&
		!matchesVersionValue(actual.dwPlatformId, requested->dwPlatformId, versionCondition(mask, 3))) {
		return ERROR_OLD_WIN_VERSION;
	}
	if ((fields & VER_BUILDNUMBER) &&
		!matchesVersionValue(actual.dwBuildNumber, requested->dwBuildNumber, versionCondition(mask, 2))) {
		return ERROR_OLD_WIN_VERSION;
	}

	struct Component {
		unsigned int bit;
		DWORD actual;
		DWORD requested;
	};
	const Component hierarchy[] = {
		{1, actual.dwMajorVersion, requested->dwMajorVersion},
		{0, actual.dwMinorVersion, requested->dwMinorVersion},
		{5, actual.wServicePackMajor, requested->wServicePackMajor},
		{4, actual.wServicePackMinor, requested->wServicePackMinor},
	};
	BYTE governing = 0;
	bool inheritRemaining = false;
	bool matched = true;
	for (const Component &component : hierarchy) {
		if (!(fields & (1u << component.bit))) {
			continue;
		}
		const BYTE supplied = versionCondition(mask, component.bit);
		BYTE effective = governing;
		if (!inheritRemaining && (governing == 0 || (governing == VER_EQUAL && isVersionRelation(supplied)))) {
			governing = effective = supplied;
		} else if (!inheritRemaining && sameVersionDirection(governing, supplied)) {
			effective = supplied;
		}
		if (!supplied) {
			inheritRemaining = true;
		}
		matched = matchesVersionValue(component.actual, component.requested, effective);
		// Equal components defer even a strict relation to the next selected
		// component. The first unequal component decides the hierarchy.
		if (component.actual != component.requested || !isVersionRelation(effective)) {
			break;
		}
	}
	return matched ? ERROR_SUCCESS : ERROR_OLD_WIN_VERSION;
}

} // namespace

namespace kernel32 {

ULONGLONG WINAPI GetEnabledXStateFeatures() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetEnabledXStateFeatures()\n");
	// Extended guest context state is not currently exposed.
	return 0;
}

BOOL WINAPI GetNumaHighestNodeNumber(PULONG highestNodeNumber) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetNumaHighestNodeNumber(%p)\n", highestNodeNumber);
	if (!highestNodeNumber) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"numa-highest-node-number"}, response)) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	uint32_t highest = 0;
	if (!reader.header(status) || (status == ERROR_SUCCESS && !reader.number(highest)) || !reader.done()) {
		setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	if (status != ERROR_SUCCESS) {
		setLastError(static_cast<DWORD>(status));
		return FALSE;
	}
	*highestNodeNumber = highest;
	return TRUE;
}

BOOL WINAPI GetNumaProcessorNodeEx(PPROCESSOR_NUMBER processor, WORD *nodeNumber) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetNumaProcessorNodeEx(%p, %p)\n", processor, nodeNumber);
	if (!processor || !nodeNumber) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	SYSTEM_INFO system{};
	GetSystemInfo(&system);
	*nodeNumber =
		processor->Group == 0 && processor->Number < system.dwNumberOfProcessors ? 0 : std::numeric_limits<WORD>::max();
	return TRUE;
}

DWORD WINAPI GetActiveProcessorCount(WORD groupNumber) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetActiveProcessorCount(%u)\n", groupNumber);
	if (groupNumber != 0 && groupNumber != 0xffff) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	const long count = sysconf(_SC_NPROCESSORS_ONLN);
	if (count <= 0 || static_cast<unsigned long>(count) > std::numeric_limits<DWORD>::max()) {
		setLastError(ERROR_GEN_FAILURE);
		return 0;
	}
	if (groupNumber == 0 && count > static_cast<long>(sizeof(DWORD_PTR) * 8)) {
		// Processor-group mapping beyond the existing mask width is unavailable.
		setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	return static_cast<DWORD>(count);
}

void WINAPI GetSystemInfo(LPSYSTEM_INFO lpSystemInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemInfo(%p)\n", lpSystemInfo);
	if (!lpSystemInfo) {
		return;
	}

	std::memset(lpSystemInfo, 0, sizeof(*lpSystemInfo));
	lpSystemInfo->wProcessorArchitecture = kSystemProcessorArchitecture;
	lpSystemInfo->dwOemId = lpSystemInfo->wProcessorArchitecture;
	lpSystemInfo->dwProcessorType = kSystemProcessorType;
	lpSystemInfo->wProcessorLevel = 6; // Retain the existing CPU-family facade.

	long pageSize = sysconf(_SC_PAGESIZE);
	if (pageSize <= 0) {
		pageSize = 4096;
	}
	lpSystemInfo->dwPageSize = static_cast<DWORD>(pageSize);

	lpSystemInfo->lpMinimumApplicationAddress = toGuestPtr(reinterpret_cast<void *>(0x00010000));
#ifdef WIBO_GUEST_64
	lpSystemInfo->lpMaximumApplicationAddress = toGuestPtr(reinterpret_cast<void *>(0x00007FFFFFFEFFFFull));
#else
	lpSystemInfo->lpMaximumApplicationAddress = toGuestPtr(reinterpret_cast<void *>(0x7FFEFFFF));
#endif

	unsigned int cpuCount = 1;
	long reported = sysconf(_SC_NPROCESSORS_ONLN);
	if (reported > 0) {
		// Match the single processor group represented by our pointer-sized
		// mask and GetLogicalProcessorInformation.
		cpuCount = static_cast<unsigned int>(std::min(reported, static_cast<long>(sizeof(DWORD_PTR) * 8)));
	}
	lpSystemInfo->dwNumberOfProcessors = cpuCount;
	lpSystemInfo->dwActiveProcessorMask = computeSystemProcessorMask(cpuCount);

	lpSystemInfo->dwAllocationGranularity = 0x10000;
}

void WINAPI GetNativeSystemInfo(LPSYSTEM_INFO lpSystemInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetNativeSystemInfo(%p)\n", lpSystemInfo);
	// IsWow64Process reports false: each guest presents a native Windows
	// architecture. The host CPU does not change that compatibility facade.
	GetSystemInfo(lpSystemInfo);
}

BOOL WINAPI GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer, PDWORD returnLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLogicalProcessorInformation(%p, %p)\n", buffer, returnLength);
	if (!returnLength) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	long reported = sysconf(_SC_NPROCESSORS_ONLN);
	unsigned int logicalCount = reported > 0 ? static_cast<unsigned int>(reported) : 1;
	logicalCount = std::min(logicalCount, static_cast<unsigned int>(sizeof(ULONG_PTR) * 8));
	const size_t recordCount = static_cast<size_t>(logicalCount) + 1;
	const size_t requiredSize = recordCount * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
	if (requiredSize > std::numeric_limits<DWORD>::max()) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}

	const DWORD required = static_cast<DWORD>(requiredSize);
	if (!buffer || *returnLength < required) {
		*returnLength = required;
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}

	std::memset(buffer, 0, required);
	for (unsigned int index = 0; index < logicalCount; ++index) {
		buffer[index].ProcessorMask = static_cast<ULONG_PTR>(1) << index;
		buffer[index].Relationship = RelationProcessorCore;
	}
	buffer[logicalCount].ProcessorMask = computeSystemProcessorMask(logicalCount);
	buffer[logicalCount].Relationship = RelationProcessorPackage;
	*returnLength = required;
	setLastError(ERROR_SUCCESS);
	return TRUE;
}

BOOL WINAPI GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP relationship,
											PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer, PDWORD returnLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLogicalProcessorInformationEx(%u, %p, %p)\n", relationship, buffer, returnLength);
	if (!returnLength) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (relationship != RelationProcessorCore && relationship != RelationProcessorPackage && relationship != RelationGroup) {
		// The single-group processor view does not include cache or NUMA discovery.
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	SYSTEM_INFO system{};
	GetSystemInfo(&system);
	const DWORD count = relationship == RelationProcessorCore ? system.dwNumberOfProcessors : 1;
	const DWORD recordSize = relationship == RelationGroup
		? offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Group) + sizeof(GROUP_RELATIONSHIP)
		: offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor) + sizeof(PROCESSOR_RELATIONSHIP);
	const DWORD required = count * recordSize;
	if (!buffer || *returnLength < required) {
		*returnLength = required;
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	std::memset(buffer, 0, required);
	for (DWORD i = 0; i < count; ++i) {
		auto *record = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(reinterpret_cast<BYTE *>(buffer) + i * recordSize);
		record->Relationship = relationship;
		record->Size = recordSize;
		if (relationship == RelationGroup) {
			record->Group.MaximumGroupCount = 1;
			record->Group.ActiveGroupCount = 1;
			record->Group.GroupInfo[0].MaximumProcessorCount = sizeof(ULONG_PTR) * 8;
			record->Group.GroupInfo[0].ActiveProcessorCount = system.dwNumberOfProcessors;
			record->Group.GroupInfo[0].ActiveProcessorMask = system.dwActiveProcessorMask;
		} else {
			record->Processor.GroupCount = 1;
			record->Processor.GroupMask[0].Mask = relationship == RelationProcessorCore
				? static_cast<ULONG_PTR>(1) << i : system.dwActiveProcessorMask;
		}
	}
	*returnLength = required;
	return TRUE;
}

void WINAPI GetSystemTime(LPSYSTEMTIME lpSystemTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemTime(%p)\n", lpSystemTime);
	if (!lpSystemTime) {
		return;
	}

	time_t now = time(nullptr);
	struct tm tmUtc{};
#if defined(_GNU_SOURCE) || defined(__APPLE__)
	gmtime_r(&now, &tmUtc);
#else
	struct tm *tmp = gmtime(&now);
	if (!tmp) {
		return;
	}
	tmUtc = *tmp;
#endif

	lpSystemTime->wYear = static_cast<WORD>(tmUtc.tm_year + 1900);
	lpSystemTime->wMonth = static_cast<WORD>(tmUtc.tm_mon + 1);
	lpSystemTime->wDayOfWeek = static_cast<WORD>(tmUtc.tm_wday);
	lpSystemTime->wDay = static_cast<WORD>(tmUtc.tm_mday);
	lpSystemTime->wHour = static_cast<WORD>(tmUtc.tm_hour);
	lpSystemTime->wMinute = static_cast<WORD>(tmUtc.tm_min);
	lpSystemTime->wSecond = static_cast<WORD>(tmUtc.tm_sec);
	lpSystemTime->wMilliseconds = 0;
}

void WINAPI GetLocalTime(LPSYSTEMTIME lpSystemTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLocalTime(%p)\n", lpSystemTime);
	if (!lpSystemTime) {
		return;
	}

	time_t now = time(nullptr);
	struct tm tmLocal{};
#if defined(_GNU_SOURCE) || defined(__APPLE__)
	localtime_r(&now, &tmLocal);
#else
	struct tm *tmp = localtime(&now);
	if (!tmp) {
		return;
	}
	tmLocal = *tmp;
#endif

	lpSystemTime->wYear = static_cast<WORD>(tmLocal.tm_year + 1900);
	lpSystemTime->wMonth = static_cast<WORD>(tmLocal.tm_mon + 1);
	lpSystemTime->wDayOfWeek = static_cast<WORD>(tmLocal.tm_wday);
	lpSystemTime->wDay = static_cast<WORD>(tmLocal.tm_mday);
	lpSystemTime->wHour = static_cast<WORD>(tmLocal.tm_hour);
	lpSystemTime->wMinute = static_cast<WORD>(tmLocal.tm_min);
	lpSystemTime->wSecond = static_cast<WORD>(tmLocal.tm_sec);
	lpSystemTime->wMilliseconds = 0;
}

void WINAPI GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemTimeAsFileTime(%p)\n", lpSystemTimeAsFileTime);
	writeCurrentFileTime(lpSystemTimeAsFileTime);
}

void WINAPI GetSystemTimePreciseAsFileTime(LPFILETIME lpSystemTimeAsFileTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemTimePreciseAsFileTime(%p)\n", lpSystemTimeAsFileTime);
	writeCurrentFileTime(lpSystemTimeAsFileTime);
}

ULONGLONG WINAPI GetTickCount64() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetTickCount64()\n");
	struct timespec ts{};
#if defined(CLOCK_BOOTTIME)
	constexpr clockid_t clock = CLOCK_BOOTTIME;
#else
	constexpr clockid_t clock = CLOCK_MONOTONIC;
#endif
	if (clock_gettime(clock, &ts) != 0) {
		std::perror("clock_gettime");
		std::exit(EXIT_FAILURE);
	}
	const ULONGLONG milliseconds =
		static_cast<ULONGLONG>(ts.tv_sec) * 1000ULL + static_cast<ULONGLONG>(ts.tv_nsec) / 1000000ULL;
	DEBUG_LOG(" -> %llu\n", static_cast<unsigned long long>(milliseconds));
	return milliseconds;
}

DWORD WINAPI GetTickCount() { return static_cast<DWORD>(GetTickCount64()); }

DWORD WINAPI GetVersion() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetVersion()\n");
	return kMajorVersion | (kMinorVersion << 8) | (5 << 16) | (kBuildNumber << 24);
}

BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA lpVersionInformation) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetVersionExA(%p)\n", lpVersionInformation);
	if (!lpVersionInformation) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	DWORD size = lpVersionInformation->dwOSVersionInfoSize;
	if (size < sizeof(OSVERSIONINFOA)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	DWORD requestSize = (size >= sizeof(OSVERSIONINFOEXA)) ? sizeof(OSVERSIONINFOEXW) : sizeof(OSVERSIONINFOW);
	OSVERSIONINFOEXW wideInfo{};
	wideInfo.dwOSVersionInfoSize = requestSize;
	NTSTATUS status = ntdll::RtlGetVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(&wideInfo));
	if (status != STATUS_SUCCESS) {
		setLastError(wibo::winErrorFromNtStatus(status));
		return FALSE;
	}

	std::memset(lpVersionInformation, 0, size);
	lpVersionInformation->dwOSVersionInfoSize = size;
	lpVersionInformation->dwMajorVersion = wideInfo.dwMajorVersion;
	lpVersionInformation->dwMinorVersion = wideInfo.dwMinorVersion;
	lpVersionInformation->dwBuildNumber = wideInfo.dwBuildNumber;
	lpVersionInformation->dwPlatformId = wideInfo.dwPlatformId;

	if (size >= sizeof(OSVERSIONINFOEXA)) {
		auto extended = reinterpret_cast<OSVERSIONINFOEXA *>(lpVersionInformation);
		extended->wServicePackMajor = wideInfo.wServicePackMajor;
		extended->wServicePackMinor = wideInfo.wServicePackMinor;
		extended->wSuiteMask = wideInfo.wSuiteMask;
		extended->wProductType = wideInfo.wProductType;
		extended->wReserved = wideInfo.wReserved;
	}

	return TRUE;
}

BOOL WINAPI GetVersionExW(LPOSVERSIONINFOW lpVersionInformation) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetVersionExW(%p)\n", lpVersionInformation);
	if (!lpVersionInformation) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	DWORD size = lpVersionInformation->dwOSVersionInfoSize;
	if (size < sizeof(OSVERSIONINFOW)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	NTSTATUS status = ntdll::RtlGetVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(lpVersionInformation));
	if (status != STATUS_SUCCESS) {
		setLastError(wibo::winErrorFromNtStatus(status));
		return FALSE;
	}

	lpVersionInformation->dwOSVersionInfoSize = size;
	return TRUE;
}

ULONGLONG WINAPI VerSetConditionMask(ULONGLONG ConditionMask, DWORD TypeMask, BYTE Condition) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VerSetConditionMask(0x%llx, 0x%x, %u)\n", ConditionMask, TypeMask, Condition);
	return ntdll::VerSetConditionMask(ConditionMask, TypeMask, Condition);
}

BOOL WINAPI VerifyVersionInfoA(LPOSVERSIONINFOEXA lpVersionInformation, DWORD dwTypeMask, ULONGLONG dwlConditionMask) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VerifyVersionInfoA(%p, 0x%x, 0x%llx)\n", lpVersionInformation, dwTypeMask, dwlConditionMask);
	const DWORD error = verifyVersionConditions(lpVersionInformation, dwTypeMask, dwlConditionMask);
	if (error != ERROR_SUCCESS) {
		setLastError(error);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI VerifyVersionInfoW(LPOSVERSIONINFOEXW lpVersionInformation, DWORD dwTypeMask, ULONGLONG dwlConditionMask) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VerifyVersionInfoW(%p, 0x%x, 0x%llx)\n", lpVersionInformation, dwTypeMask, dwlConditionMask);
	const DWORD error = verifyVersionConditions(lpVersionInformation, dwTypeMask, dwlConditionMask);
	if (error != ERROR_SUCCESS) {
		setLastError(error);
		return FALSE;
	}
	return TRUE;
}

} // namespace kernel32
