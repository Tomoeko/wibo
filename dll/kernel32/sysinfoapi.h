#pragma once

#include "minwinbase.h"
#include "types.h"

struct SYSTEM_INFO {
	union {
		DWORD dwOemId;
		struct {
			WORD wProcessorArchitecture;
			WORD wReserved;
		};
	};
	DWORD dwPageSize;
	GUEST_PTR lpMinimumApplicationAddress;
	GUEST_PTR lpMaximumApplicationAddress;
	DWORD_PTR dwActiveProcessorMask;
	DWORD dwNumberOfProcessors;
	DWORD dwProcessorType;
	DWORD dwAllocationGranularity;
	WORD wProcessorLevel;
	WORD wProcessorRevision;
};

using LPSYSTEM_INFO = SYSTEM_INFO *;

struct MEMORYSTATUSEX {
	DWORD dwLength;
	DWORD dwMemoryLoad;
	ULONGLONG ullTotalPhys;
	ULONGLONG ullAvailPhys;
	ULONGLONG ullTotalPageFile;
	ULONGLONG ullAvailPageFile;
	ULONGLONG ullTotalVirtual;
	ULONGLONG ullAvailVirtual;
	ULONGLONG ullAvailExtendedVirtual;
};
static_assert(sizeof(MEMORYSTATUSEX) == 64);

enum LOGICAL_PROCESSOR_RELATIONSHIP {
	RelationProcessorCore,
	RelationNumaNode,
	RelationCache,
	RelationProcessorPackage,
	RelationGroup,
	RelationAll = 0xffff
};

struct CACHE_DESCRIPTOR {
	BYTE Level;
	BYTE Associativity;
	WORD LineSize;
	DWORD Size;
	DWORD Type;
};

struct SYSTEM_LOGICAL_PROCESSOR_INFORMATION {
	ULONG_PTR ProcessorMask;
	LOGICAL_PROCESSOR_RELATIONSHIP Relationship;
	union {
		struct {
			BYTE Flags;
		} ProcessorCore;
		struct {
			DWORD NodeNumber;
		} NumaNode;
		CACHE_DESCRIPTOR Cache;
		ULONGLONG Reserved[2];
	};
};

using PSYSTEM_LOGICAL_PROCESSOR_INFORMATION = SYSTEM_LOGICAL_PROCESSOR_INFORMATION *;

struct GROUP_AFFINITY {
	ULONG_PTR Mask;
	WORD Group;
	WORD Reserved[3];
};

struct PROCESSOR_RELATIONSHIP {
	BYTE Flags;
	BYTE EfficiencyClass;
	BYTE Reserved[20];
	WORD GroupCount;
	GROUP_AFFINITY GroupMask[1];
};

struct PROCESSOR_GROUP_INFO {
	BYTE MaximumProcessorCount;
	BYTE ActiveProcessorCount;
	BYTE Reserved[38];
	ULONG_PTR ActiveProcessorMask;
};

struct GROUP_RELATIONSHIP {
	WORD MaximumGroupCount;
	WORD ActiveGroupCount;
	BYTE Reserved[20];
	PROCESSOR_GROUP_INFO GroupInfo[1];
};

struct SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX {
	LOGICAL_PROCESSOR_RELATIONSHIP Relationship;
	DWORD Size;
	union {
		PROCESSOR_RELATIONSHIP Processor;
		GROUP_RELATIONSHIP Group;
	};
};

using PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX = SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *;

struct OSVERSIONINFOA {
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformId;
	char szCSDVersion[128];
};

using LPOSVERSIONINFOA = OSVERSIONINFOA *;

struct OSVERSIONINFOW {
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformId;
	WCHAR szCSDVersion[128];
};

using LPOSVERSIONINFOW = OSVERSIONINFOW *;

struct OSVERSIONINFOEXA : OSVERSIONINFOA {
	WORD wServicePackMajor;
	WORD wServicePackMinor;
	WORD wSuiteMask;
	BYTE wProductType;
	BYTE wReserved;
};

using LPOSVERSIONINFOEXA = OSVERSIONINFOEXA *;

struct OSVERSIONINFOEXW : OSVERSIONINFOW {
	WORD wServicePackMajor;
	WORD wServicePackMinor;
	WORD wSuiteMask;
	BYTE wProductType;
	BYTE wReserved;
};

using LPOSVERSIONINFOEXW = OSVERSIONINFOEXW *;

enum : DWORD {
	VER_MINORVERSION = 0x01,
	VER_MAJORVERSION = 0x02,
	VER_BUILDNUMBER = 0x04,
	VER_PLATFORMID = 0x08,
	VER_SERVICEPACKMINOR = 0x10,
	VER_SERVICEPACKMAJOR = 0x20,
	VER_SUITENAME = 0x40,
	VER_PRODUCT_TYPE = 0x80,
};

enum : BYTE {
	VER_EQUAL = 1,
	VER_GREATER = 2,
	VER_GREATER_EQUAL = 3,
	VER_LESS = 4,
	VER_LESS_EQUAL = 5,
	VER_AND = 6,
	VER_OR = 7,
};

namespace kernel32 {
BOOL WINAPI GlobalMemoryStatusEx(MEMORYSTATUSEX *status);

DWORD WINAPI GetActiveProcessorCount(WORD groupNumber);
void WINAPI GetSystemInfo(LPSYSTEM_INFO lpSystemInfo);
void WINAPI GetNativeSystemInfo(LPSYSTEM_INFO lpSystemInfo);
BOOL WINAPI GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer, PDWORD returnLength);
BOOL WINAPI GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP relationship,
											PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer, PDWORD returnLength);
void WINAPI GetSystemTime(LPSYSTEMTIME lpSystemTime);
void WINAPI GetLocalTime(LPSYSTEMTIME lpSystemTime);
void WINAPI GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime);
DWORD WINAPI GetTickCount();
ULONGLONG WINAPI GetTickCount64();
DWORD WINAPI GetVersion();
BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA lpVersionInformation);
BOOL WINAPI GetVersionExW(LPOSVERSIONINFOW lpVersionInformation);
ULONGLONG WINAPI VerSetConditionMask(ULONGLONG ConditionMask, DWORD TypeMask, BYTE Condition);
BOOL WINAPI VerifyVersionInfoA(LPOSVERSIONINFOEXA lpVersionInformation, DWORD dwTypeMask, ULONGLONG dwlConditionMask);
BOOL WINAPI VerifyVersionInfoW(LPOSVERSIONINFOEXW lpVersionInformation, DWORD dwTypeMask, ULONGLONG dwlConditionMask);

} // namespace kernel32
