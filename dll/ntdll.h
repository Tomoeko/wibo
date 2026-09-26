#pragma once

#include "types.h"

struct RTL_CRITICAL_SECTION;

using PIO_APC_ROUTINE = PVOID;

typedef struct _IO_STATUS_BLOCK {
	union {
		NTSTATUS Status;
		GUEST_PTR Pointer;
	};
	ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

enum PROCESSINFOCLASS {
	ProcessBasicInformation = 0,
	ProcessWow64Information = 26,
	ProcessImageFileName = 27,
};

struct RTL_OSVERSIONINFOW {
	ULONG dwOSVersionInfoSize;
	ULONG dwMajorVersion;
	ULONG dwMinorVersion;
	ULONG dwBuildNumber;
	ULONG dwPlatformId;
	WCHAR szCSDVersion[128];
};

enum WINE_DEBUG_CLASS {
	WINE_DBCL_FIXME,
	WINE_DBCL_ERR,
	WINE_DBCL_WARN,
	WINE_DBCL_TRACE,
	WINE_DBCL_INIT = 7,
};

struct WINE_DEBUG_CHANNEL {
	BYTE flags;
	char name[15];
};

using PRTL_OSVERSIONINFOW = RTL_OSVERSIONINFOW *;

namespace ntdll {

ULONG WINAPI RtlNtStatusToDosError(NTSTATUS status);
NTSTATUS WINAPI NtDeviceIoControlFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apcRoutine, PVOID apcContext,
									  PIO_STATUS_BLOCK ioStatus, ULONG control, PVOID input, ULONG inputLength,
									  PVOID output, ULONG outputLength);

PVOID CDECL memset(PVOID dest, int ch, SIZE_T count);
BYTE CDECL __wine_dbg_get_channel_flags(WINE_DEBUG_CHANNEL *channel);
int CDECL __wine_dbg_header(WINE_DEBUG_CLASS debugClass, WINE_DEBUG_CHANNEL *channel, const char *function);
int CDECL __wine_dbg_output(const char *str);
const char *CDECL __wine_dbg_strdup(const char *str);
NTSTATUS WINAPI NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
						   PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset,
						   PULONG Key);
NTSTATUS WINAPI NtWriteFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
							PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset,
							PULONG Key);
NTSTATUS WINAPI NtAllocateVirtualMemory(HANDLE ProcessHandle, guest_ptr<> *BaseAddress, ULONG_PTR ZeroBits,
										PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect);
NTSTATUS WINAPI NtProtectVirtualMemory(HANDLE ProcessHandle, guest_ptr<> *BaseAddress, PSIZE_T NumberOfBytesToProtect,
									   ULONG NewAccessProtection, PULONG OldAccessProtection);
NTSTATUS WINAPI NtSetInformationFile(HANDLE file, PIO_STATUS_BLOCK ioStatus, PVOID information, ULONG length,
									 FILE_INFORMATION_CLASS informationClass);
NTSTATUS WINAPI NtQueryInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
									   ULONG Length, FILE_INFORMATION_CLASS FileInformationClass);
NTSTATUS WINAPI NtQuerySystemTime(PLARGE_INTEGER SystemTime);
BOOLEAN WINAPI RtlTimeToSecondsSince1970(PLARGE_INTEGER Time, PULONG ElapsedSeconds);
VOID WINAPI RtlInitializeBitMap(PRTL_BITMAP BitMapHeader, PULONG BitMapBuffer, ULONG SizeOfBitMap);
VOID WINAPI RtlSetBits(PRTL_BITMAP BitMapHeader, ULONG StartingIndex, ULONG NumberToSet);
BOOLEAN WINAPI RtlAreBitsSet(PRTL_BITMAP BitMapHeader, ULONG StartingIndex, ULONG Length);
BOOLEAN WINAPI RtlAreBitsClear(PRTL_BITMAP BitMapHeader, ULONG StartingIndex, ULONG Length);
BOOL WINAPI RtlIsCriticalSectionLockedByThread(RTL_CRITICAL_SECTION *CriticalSection);
NTSTATUS WINAPI RtlGetVersion(PRTL_OSVERSIONINFOW lpVersionInformation);
ULONGLONG WINAPI VerSetConditionMask(ULONGLONG ConditionMask, DWORD TypeMask, BYTE Condition);
NTSTATUS WINAPI NtQueryInformationProcess(HANDLE ProcessHandle, PROCESSINFOCLASS ProcessInformationClass,
										  PVOID ProcessInformation, ULONG ProcessInformationLength,
										  PULONG ReturnLength);
NTSTATUS WINAPI LdrAddRefDll(ULONG Flags, HMODULE Module);

} // namespace ntdll
