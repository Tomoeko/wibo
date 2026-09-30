#pragma once

#include "minwinbase.h"
#include "thread_context.h"
#include "types.h"

struct PROCESS_INFORMATION {
	HANDLE hProcess;
	HANDLE hThread;
	DWORD dwProcessId;
	DWORD dwThreadId;
};

using PPROCESS_INFORMATION = PROCESS_INFORMATION *;
using LPPROCESS_INFORMATION = PROCESS_INFORMATION *;

struct PROCESSOR_NUMBER {
	WORD Group;
	BYTE Number;
	BYTE Reserved;
};
static_assert(sizeof(PROCESSOR_NUMBER) == 4);
static_assert(alignof(PROCESSOR_NUMBER) == 2);
static_assert(offsetof(PROCESSOR_NUMBER, Group) == 0);
static_assert(offsetof(PROCESSOR_NUMBER, Number) == 2);
static_assert(offsetof(PROCESSOR_NUMBER, Reserved) == 3);
using PPROCESSOR_NUMBER = PROCESSOR_NUMBER *;

struct STARTUPINFOA {
	DWORD cb;
	GUEST_PTR lpReserved;
	GUEST_PTR lpDesktop;
	GUEST_PTR lpTitle;
	DWORD dwX;
	DWORD dwY;
	DWORD dwXSize;
	DWORD dwYSize;
	DWORD dwXCountChars;
	DWORD dwYCountChars;
	DWORD dwFillAttribute;
	DWORD dwFlags;
	WORD wShowWindow;
	WORD cbReserved2;
	GUEST_PTR lpReserved2;
	HANDLE hStdInput;
	HANDLE hStdOutput;
	HANDLE hStdError;
};

using LPSTARTUPINFOA = STARTUPINFOA *;

struct STARTUPINFOW {
	DWORD cb;
	GUEST_PTR lpReserved;
	GUEST_PTR lpDesktop;
	GUEST_PTR lpTitle;
	DWORD dwX;
	DWORD dwY;
	DWORD dwXSize;
	DWORD dwYSize;
	DWORD dwXCountChars;
	DWORD dwYCountChars;
	DWORD dwFillAttribute;
	DWORD dwFlags;
	WORD wShowWindow;
	WORD cbReserved2;
	GUEST_PTR lpReserved2;
	HANDLE hStdInput;
	HANDLE hStdOutput;
	HANDLE hStdError;
};

using LPSTARTUPINFOW = STARTUPINFOW *;

struct PROC_THREAD_ATTRIBUTE_LIST;
using LPPROC_THREAD_ATTRIBUTE_LIST = PROC_THREAD_ATTRIBUTE_LIST *;

struct STARTUPINFOEXA {
	STARTUPINFOA StartupInfo;
	GUEST_PTR lpAttributeList;
};

constexpr DWORD TLS_OUT_OF_INDEXES = 0xFFFFFFFFu;
constexpr DWORD PROCESS_TERMINATE = 0x0001;
constexpr DWORD PROCESS_VM_OPERATION = 0x0008;
constexpr DWORD PROCESS_VM_READ = 0x0010;
constexpr DWORD PROCESS_QUERY_INFORMATION = 0x0400;
constexpr DWORD PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
constexpr DWORD PROCESS_ALL_ACCESS = STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0xFFFF;
constexpr DWORD NORMAL_PRIORITY_CLASS = 0x00000020;
constexpr DWORD THREAD_SUSPEND_RESUME = 0x0002;
constexpr DWORD THREAD_SET_INFORMATION = 0x0020;
constexpr DWORD THREAD_QUERY_INFORMATION = 0x0040;
constexpr DWORD THREAD_SET_LIMITED_INFORMATION = 0x0400;
constexpr DWORD THREAD_QUERY_LIMITED_INFORMATION = 0x0800;
constexpr DWORD THREAD_ALL_ACCESS = STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0xFFFF;

typedef DWORD(_CC_STDCALL *LPTHREAD_START_ROUTINE)(LPVOID);
typedef void(_CC_STDCALL *PAPCFUNC)(ULONG_PTR);

namespace kernel32 {

BOOL WINAPI GetSystemTimes(FILETIME *idleTime, FILETIME *kernelTime, FILETIME *userTime);
BOOL WINAPI SwitchToThread();

BOOL WINAPI QueueUserWorkItem(LPTHREAD_START_ROUTINE function, PVOID context, ULONG flags);
DWORD WINAPI QueueUserAPC(PAPCFUNC callback, HANDLE thread, ULONG_PTR argument);

BOOL WINAPI InitializeProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList, DWORD dwAttributeCount,
											 DWORD dwFlags, SIZE_T *lpSize);
BOOL WINAPI UpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList, DWORD dwFlags, DWORD_PTR Attribute,
									 PVOID lpValue, SIZE_T cbSize, PVOID lpPreviousValue, SIZE_T *lpReturnSize);
void WINAPI DeleteProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList);

HANDLE WINAPI GetCurrentProcess();
void WINAPI FlushProcessWriteBuffers();
BOOL WINAPI FlushInstructionCache(HANDLE process, LPCVOID address, SIZE_T size);
DWORD WINAPI GetCurrentProcessId();
DWORD WINAPI GetCurrentThreadId();
DWORD WINAPI GetCurrentProcessorNumber();
void WINAPI GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER ProcNumber);
WORD WINAPI GetActiveProcessorGroupCount();
HANDLE WINAPI GetCurrentThread();
HANDLE WINAPI OpenThread(DWORD access, BOOL inherit, DWORD threadId);
HANDLE WINAPI OpenProcess(DWORD access, BOOL inherit, DWORD processId);
DWORD WINAPI GetThreadId(HANDLE Thread);
BOOL WINAPI IsProcessorFeaturePresent(DWORD ProcessorFeature);
BOOL WINAPI GetProcessAffinityMask(HANDLE hProcess, PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask);
BOOL WINAPI SetProcessAffinityMask(HANDLE hProcess, DWORD_PTR dwProcessAffinityMask);
DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask);
BOOL WINAPI GetThreadContext(HANDLE hThread, LPCONTEXT context);
BOOL WINAPI SetThreadContext(HANDLE hThread, const CONTEXT *context);
DWORD WINAPI SuspendThread(HANDLE hThread);
DWORD WINAPI ResumeThread(HANDLE hThread);
void WINAPI ExitProcess(UINT uExitCode);
BOOL WINAPI TerminateProcess(HANDLE hProcess, UINT uExitCode);
BOOL WINAPI GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode);
DWORD WINAPI TlsAlloc();
BOOL WINAPI TlsFree(DWORD dwTlsIndex);
LPVOID WINAPI TlsGetValue(DWORD dwTlsIndex);
BOOL WINAPI TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue);
HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
						   LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags,
						   LPDWORD lpThreadId);
[[noreturn]] void WINAPI ExitThread(DWORD dwExitCode);
BOOL WINAPI GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode);
BOOL WINAPI SetThreadPriority(HANDLE hThread, int nPriority);
BOOL WINAPI SetThreadPriorityBoost(HANDLE hThread, BOOL bDisablePriorityBoost);
DWORD WINAPI SetThreadIdealProcessor(HANDLE hThread, DWORD dwIdealProcessor);
BOOL WINAPI GetThreadIdealProcessorEx(HANDLE thread, PPROCESSOR_NUMBER ideal);
BOOL WINAPI SetThreadIdealProcessorEx(HANDLE thread, PPROCESSOR_NUMBER ideal, PPROCESSOR_NUMBER previous);
int WINAPI GetThreadPriority(HANDLE hThread);
DWORD WINAPI GetPriorityClass(HANDLE hProcess);
BOOL WINAPI GetThreadTimes(HANDLE hThread, FILETIME *lpCreationTime, FILETIME *lpExitTime, FILETIME *lpKernelTime,
						   FILETIME *lpUserTime);
BOOL WINAPI GetProcessTimes(HANDLE process, FILETIME *creationTime, FILETIME *exitTime, FILETIME *kernelTime,
							FILETIME *userTime);
BOOL WINAPI CreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes,
						   LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags,
						   LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo,
						   LPPROCESS_INFORMATION lpProcessInformation);
BOOL WINAPI CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes,
						   LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags,
						   LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
						   LPPROCESS_INFORMATION lpProcessInformation);
void WINAPI GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo);
void WINAPI GetStartupInfoW(LPSTARTUPINFOW lpStartupInfo);
BOOL WINAPI SetThreadStackGuarantee(PULONG StackSizeInBytes);

} // namespace kernel32
