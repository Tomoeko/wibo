#include "processthreadsapi.h"
#include "ws2/internal.h"

#include "common.h"
#include "context.h"
#include "directory_changes.h"
#include "errors.h"
#include "files.h"
#include "handles.h"
#include "internal.h"
#include "kernel32.h"
#include "kernel32_trampolines.h"
#include "modules.h"
#include "processes.h"
#include "strutil.h"
#include "timeutil.h"
#include "tls.h"
#include "types.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#endif

namespace {

using kernel32::ThreadObject;

DWORD_PTR g_processAffinityMask = 0;
bool g_processAffinityMaskInitialized = false;

const FILETIME kDefaultThreadFileTime = {static_cast<DWORD>(UNIX_TIME_ZERO & 0xFFFFFFFFULL),
										 static_cast<DWORD>(UNIX_TIME_ZERO >> 32)};

constexpr DWORD STARTF_USESHOWWINDOW = 0x00000001;
constexpr DWORD STARTF_USESTDHANDLES = 0x00000100;
constexpr WORD SW_SHOWNORMAL = 1;

constexpr DWORD EXTENDED_STARTUPINFO_PRESENT = 0x00080000;
constexpr DWORD_PTR PROC_THREAD_ATTRIBUTE_HANDLE_LIST = 0x00020002;

struct ProcessAttribute {
	DWORD_PTR key;
	GUEST_PTR value;
	SIZE_T size;
};

struct ProcessAttributeList {
	DWORD capacity;
	DWORD count;
};

FILETIME fileTimeFromTimeval(const struct timeval &value) {
	uint64_t total = 0;
	if (value.tv_sec > 0 || value.tv_usec > 0) {
		total = static_cast<uint64_t>(value.tv_sec) * 10000000ULL + static_cast<uint64_t>(value.tv_usec) * 10ULL;
	}
	return fileTimeFromDuration(total);
}

FILETIME fileTimeFromTimespec(const struct timespec &value) {
	uint64_t total = 0;
	if (value.tv_sec > 0 || value.tv_nsec > 0) {
		total = static_cast<uint64_t>(value.tv_sec) * 10000000ULL + static_cast<uint64_t>(value.tv_nsec) / 100ULL;
	}
	return fileTimeFromDuration(total);
}

DWORD_PTR computeSystemAffinityMask() {
	long reported = sysconf(_SC_NPROCESSORS_ONLN);
	if (reported <= 0) {
		reported = 1;
	}
	const auto bitCount = static_cast<unsigned int>(std::numeric_limits<DWORD_PTR>::digits);
	const auto usable = static_cast<unsigned int>(reported);
	if (usable >= bitCount) {
		return static_cast<DWORD_PTR>(~static_cast<DWORD_PTR>(0));
	}
	return (static_cast<DWORD_PTR>(1) << usable) - 1;
}

template <typename StartupInfo> void populateStartupInfo(StartupInfo *info) {
	if (!info) {
		return;
	}
	std::memset(info, 0, sizeof(StartupInfo));
	info->cb = sizeof(StartupInfo);
	info->dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	info->wShowWindow = SW_SHOWNORMAL;
	info->cbReserved2 = 0;
	info->lpReserved2 = GUEST_NULL;
	info->hStdInput = files::getStdHandle(STD_INPUT_HANDLE);
	info->hStdOutput = files::getStdHandle(STD_OUTPUT_HANDLE);
	info->hStdError = files::getStdHandle(STD_ERROR_HANDLE);
}

thread_local ThreadObject *g_currentThreadObject = nullptr;
std::mutex g_threadRegistryMutex;
std::unordered_map<DWORD, Pin<ThreadObject>> g_threadRegistry;

void registerThread(ThreadObject *object) {
	std::lock_guard lock(g_threadRegistryMutex);
	g_threadRegistry.insert_or_assign(object->threadId, Pin<ThreadObject>::acquire(object));
}

void retireThread(ThreadObject *object) {
	Pin<ThreadObject> retired;
	{
		std::lock_guard registryLock(g_threadRegistryMutex);
		std::lock_guard objectLock(object->m);
		if (object->signaled && object->handleCount.load(std::memory_order_relaxed) == 0) {
			auto it = g_threadRegistry.find(object->threadId);
			if (it != g_threadRegistry.end() && it->second.get() == object) {
				retired = std::move(it->second);
				g_threadRegistry.erase(it);
			}
		}
	}
}

HANDLE allocateThreadHandle(Pin<ThreadObject> object, DWORD access, DWORD flags) {
	// Opening and the final-handle retirement share a lock so an exit cannot
	// retire the ID between finding its object and allocating the new handle.
	std::lock_guard lock(g_threadRegistryMutex);
	g_threadRegistry.insert_or_assign(object->threadId, object.clone());
	return wibo::handles().alloc(std::move(object), access, flags);
}

struct ThreadStartData {
	ThreadObject *obj;
	std::function<DWORD()> entry;
};

size_t defaultThreadStackReserve() {
	constexpr size_t kWindowsDefaultStackReserve = 1024 * 1024;
	if (wibo::mainModule && wibo::mainModule->executable && wibo::mainModule->executable->stackReserveSize != 0) {
		return wibo::mainModule->executable->stackReserveSize;
	}
	return kWindowsDefaultStackReserve;
}

size_t pthreadStackReserve(SIZE_T requestedSize, DWORD creationFlags) {
	constexpr DWORD STACK_SIZE_PARAM_IS_A_RESERVATION = 0x00010000;
	size_t reserve = defaultThreadStackReserve();
	if (requestedSize != 0) {
		if ((creationFlags & STACK_SIZE_PARAM_IS_A_RESERVATION) != 0) {
			reserve = requestedSize;
		} else {
			// Windows interprets this form as the initial commit size while
			// retaining at least the image's default reserve.
			reserve = std::max(reserve, static_cast<size_t>(requestedSize));
		}
	}
#ifdef PTHREAD_STACK_MIN
	reserve = std::max(reserve, static_cast<size_t>(PTHREAD_STACK_MIN));
#endif
	long reportedPageSize = sysconf(_SC_PAGESIZE);
	size_t pageSize = reportedPageSize > 0 ? static_cast<size_t>(reportedPageSize) : 4096;
	if (reserve > std::numeric_limits<size_t>::max() - (pageSize - 1)) {
		return 0;
	}
	return (reserve + pageSize - 1) & ~(pageSize - 1);
}

void threadCleanup(void *param) {
	ThreadObject *obj = static_cast<ThreadObject *>(param);
	if (!obj) {
		return;
	}
	ws2::detail::cancelSocketIoForThread(pthread_self());
	kernel32::cancelDirectoryIoForThread(pthread_self());
	kernel32::closeApcState();
	wibo::notifyDllThreadDetach();
	wibo::uninstallTebForCurrentThread();
	{
		std::lock_guard lk(obj->m);
		obj->signaled = true;
	}
	retireThread(obj);
	g_currentThreadObject = nullptr;
	// TODO: mark mutexes owned by this thread as abandoned
	obj->cv.notify_all();
	obj->notifyWaiters(false);
	detail::deref(obj);
}

void *threadTrampoline(void *param) {
	wibo::prepareGuestWorkerSignalMask();

	// We ref'd the ThreadObject when constructing ThreadStartData,
	// so we need to deref it when done. (Either normal exit or via pthread_cleanup)
	ThreadStartData *dataPtr = static_cast<ThreadStartData *>(param);
	ThreadStartData data = std::move(*dataPtr);
	delete dataPtr;

	kernel32::captureThreadSelectors(*data.obj);
	g_currentThreadObject = data.obj;
	kernel32::installApcState(data.obj->apc);

	// Install TIB
	TEB *threadTib = wibo::allocateTib();
	wibo::initializeTibStackInfo(threadTib);
	if (!wibo::installTibForCurrentThread(threadTib)) {
		fprintf(stderr, "!!! Failed to install TIB for new thread\n");
		wibo::destroyTib(threadTib);
		threadTib = nullptr;
	}

	// Wait until resumed (if suspended at start)
	{
		std::unique_lock lk(data.obj->m);
		data.obj->tib = threadTib;
		data.obj->threadId = wibo::getThreadId();
	}
	registerThread(data.obj);
	{
		std::unique_lock lk(data.obj->m);
		data.obj->initialized = true;
		data.obj->cv.notify_all();
		if (data.obj->suspendCount) {
			DEBUG_LOG("Thread is suspended at start; waiting...\n");
			data.obj->cv.wait(lk, [&] { return data.obj->suspendCount == 0; });
		}
	}

	wibo::notifyDllThreadAttach();
	kernel32::dispatchPendingApcs();
	DEBUG_LOG("Calling thread entry\n");
	DWORD result = 0;
	if (data.entry) {
		result = data.entry();
	}
	DEBUG_LOG("Thread exiting with code %u\n", result);
	{
		std::lock_guard lk(data.obj->m);
		data.obj->exitCode = result;
	}
	threadCleanup(data.obj);
	return nullptr;
}

} // namespace

namespace kernel32 {

BOOL WINAPI FlushInstructionCache(HANDLE process, LPCVOID address, SIZE_T size) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FlushInstructionCache(%p, %p, %zu)\n", process, address, size);
	if (!isPseudoCurrentProcessHandle(process)) {
		auto object = wibo::handles().getAs<ProcessObject>(process);
		if (!object) {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
		if (object->pid != getpid()) {
			setLastError(ERROR_NOT_SUPPORTED);
			return FALSE;
		}
	}
	if (address && size) {
		const uintptr_t start = reinterpret_cast<uintptr_t>(address);
		if (size > std::numeric_limits<uintptr_t>::max() - start) {
			setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
#ifdef __APPLE__
		sys_icache_invalidate(const_cast<void *>(address), size);
#else
		__builtin___clear_cache(reinterpret_cast<char *>(start), reinterpret_cast<char *>(start + size));
#endif
	}
	// Both supported x86 architectures have coherent caches. Serialize the
	// instruction stream after writes, including requests covering all addresses.
	unsigned eax, ebx, ecx, edx;
	asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0) : "memory");
	return TRUE;
}

void ThreadObject::onLastHandleClosed() noexcept { retireThread(this); }

void initializeMainThreadObject() {
	if (g_currentThreadObject)
		return;
	auto object = make_pin<ThreadObject>(pthread_self());
	object->threadId = wibo::getThreadId();
	captureThreadSelectors(*object);
	object->initialized = true;
	object->tib = currentThreadTeb;
	object->ownsTib = false; // The main TEB is owned by the process-entry scope.
	object->apc = currentApcState();
	registerThread(object.get());
	g_currentThreadObject = object.release();
}

Pin<ThreadObject> currentThreadObject() { return Pin<ThreadObject>::acquire(g_currentThreadObject); }

HANDLE WINAPI OpenThread(DWORD access, BOOL inherit, DWORD threadId) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("OpenThread(0x%x, %d, %u)\n", access, inherit, threadId);
	if (access & ~THREAD_ALL_ACCESS) {
		setLastError(ERROR_NOT_SUPPORTED);
		return NO_HANDLE;
	}
	std::lock_guard lock(g_threadRegistryMutex);
	auto it = g_threadRegistry.find(threadId);
	if (it == g_threadRegistry.end()) {
		setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}
	if (access & THREAD_QUERY_INFORMATION)
		access |= THREAD_QUERY_LIMITED_INFORMATION; // Query information also grants the limited query right.
	if (access & THREAD_SET_INFORMATION)
		access |= THREAD_SET_LIMITED_INFORMATION;
	return wibo::handles().alloc(it->second.clone(), access, inherit ? HANDLE_FLAG_INHERIT : 0);
}

BOOL WINAPI InitializeProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList, DWORD dwAttributeCount,
											 DWORD dwFlags, SIZE_T *lpSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("InitializeProcThreadAttributeList(%p, %u, %u, %p)\n", lpAttributeList, dwAttributeCount, dwFlags, lpSize);
	constexpr SIZE_T headerSize = sizeof(ProcessAttributeList);
	if (!lpSize || dwFlags ||
		(dwAttributeCount && sizeof(ProcessAttribute) > (std::numeric_limits<SIZE_T>::max() - headerSize) / dwAttributeCount)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const SIZE_T required = headerSize + static_cast<SIZE_T>(dwAttributeCount) * sizeof(ProcessAttribute);
	const SIZE_T available = lpAttributeList ? *lpSize : 0;
	*lpSize = required;
	if (!lpAttributeList || available < required) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	auto *list = reinterpret_cast<ProcessAttributeList *>(lpAttributeList);
	list->capacity = dwAttributeCount;
	list->count = 0;
	return TRUE;
}

BOOL WINAPI UpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList, DWORD dwFlags, DWORD_PTR Attribute,
									 PVOID lpValue, SIZE_T cbSize, PVOID lpPreviousValue, SIZE_T *lpReturnSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("UpdateProcThreadAttribute(%p, %u, 0x%llx, %p, %llu, %p, %p)\n", lpAttributeList, dwFlags,
			  static_cast<unsigned long long>(Attribute), lpValue, static_cast<unsigned long long>(cbSize),
			  lpPreviousValue, lpReturnSize);
	if (!lpAttributeList || dwFlags || lpPreviousValue || lpReturnSize || !lpValue || !cbSize) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (Attribute != PROC_THREAD_ATTRIBUTE_HANDLE_LIST) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	if (cbSize % sizeof(HANDLE)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	auto *list = reinterpret_cast<ProcessAttributeList *>(lpAttributeList);
	auto *attributes = reinterpret_cast<ProcessAttribute *>(list + 1);
	for (DWORD i = 0; i < list->count; ++i) {
		if (attributes[i].key == Attribute) {
			setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
	}
	if (list->count >= list->capacity) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	attributes[list->count++] = {Attribute, toGuestPtr(lpValue), cbSize};
	return TRUE;
}

void WINAPI DeleteProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DeleteProcThreadAttributeList(%p)\n", lpAttributeList);
	if (lpAttributeList) {
		auto *list = reinterpret_cast<ProcessAttributeList *>(lpAttributeList);
		list->capacity = 0;
		list->count = 0;
	}
}

BOOL WINAPI IsProcessorFeaturePresent(DWORD ProcessorFeature) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsProcessorFeaturePresent(%u)\n", ProcessorFeature);
	if (ProcessorFeature == 0) { // PF_FLOATING_POINT_PRECISION_ERRATA
		return FALSE;
	}
	if (ProcessorFeature == 1) { // PF_FLOATING_POINT_EMULATED
		return FALSE;
	}
	if (ProcessorFeature == 10) { // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
		return TRUE;
	}
	if (ProcessorFeature == 23) { // PF_FASTFAIL_AVAILABLE (__fastfail)
		return FALSE;
	}
	DEBUG_LOG("  IsProcessorFeaturePresent: unknown feature %u, returning TRUE\n", ProcessorFeature);
	return TRUE;
}

HANDLE WINAPI GetCurrentProcess() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetCurrentProcess() -> %d\n", kPseudoCurrentProcessHandleValue);
	return kPseudoCurrentProcessHandleValue;
}

DWORD WINAPI GetCurrentProcessId() {
	HOST_CONTEXT_GUARD();
	DWORD pid = static_cast<DWORD>(getpid());
	DEBUG_LOG("GetCurrentProcessId() -> %u\n", pid);
	return pid;
}
HANDLE WINAPI OpenProcess(DWORD access, BOOL inherit, DWORD processId) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("OpenProcess(0x%x, %d, %u)\n", access, inherit, processId);
	if (!processId || processId > static_cast<DWORD>(INT_MAX)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}
	if (access & ~PROCESS_ALL_ACCESS) {
		setLastError(ERROR_NOT_SUPPORTED);
		return NO_HANDLE;
	}
	const auto pid = static_cast<pid_t>(processId);
	auto process = wibo::handles().findAs<ProcessObject>([&](const auto *object) { return object->pid == pid; });
	if (!process) {
		if (pid == getpid())
			process = make_pin<ProcessObject>(pid, -1);
		else
			process = wibo::processes().findProcess(pid);
	}
	if (!process) {
		const int error = wibo::processes().openProcess(pid, process);
		if (error) {
			DEBUG_LOG("OpenProcess: native monitoring for pid %d failed: %s\n", pid, strerror(error));
			setLastError(error == EACCES || error == EPERM	  ? ERROR_ACCESS_DENIED
						 : error == ESRCH || error == EINVAL  ? ERROR_INVALID_PARAMETER
						 : error == ENOMEM || error == EMFILE ? ERROR_NOT_ENOUGH_MEMORY
															  : ERROR_NOT_SUPPORTED);
			return NO_HANDLE;
		}
	}
	if (access & PROCESS_QUERY_INFORMATION)
		access |= PROCESS_QUERY_LIMITED_INFORMATION;
	return wibo::handles().alloc(std::move(process), access, inherit ? HANDLE_FLAG_INHERIT : 0);
}

DWORD WINAPI GetCurrentThreadId() {
	HOST_CONTEXT_GUARD();
	DWORD threadId = wibo::getThreadId();
	DEBUG_LOG("GetCurrentThreadId() -> %u\n", threadId);
	return threadId;
}

HANDLE WINAPI GetCurrentThread() {
	HOST_CONTEXT_GUARD();
	HANDLE pseudoHandle = reinterpret_cast<HANDLE>(kPseudoCurrentThreadHandleValue);
	DEBUG_LOG("GetCurrentThread() -> %p\n", pseudoHandle);
	return pseudoHandle;
}

DWORD WINAPI GetThreadId(HANDLE Thread) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetThreadId(%p)\n", Thread);
	if (isPseudoCurrentThreadHandle(Thread)) {
		return wibo::getThreadId();
	}
	HandleMeta metadata{};
	Pin<ThreadObject> obj = wibo::handles().getAs<ThreadObject>(Thread, &metadata);
	if (!obj) {
		setLastError(ERROR_INVALID_HANDLE);
		return 0;
	}
	if (!(metadata.grantedAccess & (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION))) {
		setLastError(ERROR_ACCESS_DENIED);
		return 0;
	}
	return obj->threadId;
}

BOOL WINAPI GetProcessAffinityMask(HANDLE hProcess, PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetProcessAffinityMask(%p, %p, %p)\n", hProcess, lpProcessAffinityMask, lpSystemAffinityMask);
	if (!lpProcessAffinityMask || !lpSystemAffinityMask) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (!isPseudoCurrentProcessHandle(hProcess)) {
		auto obj = wibo::handles().getAs<ProcessObject>(hProcess);
		if (!obj) {
			setLastError(ERROR_INVALID_HANDLE);
			return 0;
		}
	}

	DWORD_PTR systemMask = computeSystemAffinityMask();
	if (!g_processAffinityMaskInitialized) {
		g_processAffinityMask = systemMask;
		g_processAffinityMaskInitialized = true;
	}
	DWORD_PTR processMask = g_processAffinityMask & systemMask;
	if (processMask == 0) {
		processMask = systemMask == 0 ? 1 : systemMask;
	}

	*lpProcessAffinityMask = processMask;
	*lpSystemAffinityMask = systemMask == 0 ? 1 : systemMask;
	return TRUE;
}

BOOL WINAPI SetProcessAffinityMask(HANDLE hProcess, DWORD_PTR dwProcessAffinityMask) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetProcessAffinityMask(%p, 0x%lx)\n", hProcess, static_cast<unsigned long>(dwProcessAffinityMask));
	if (dwProcessAffinityMask == 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (!isPseudoCurrentProcessHandle(hProcess)) {
		auto obj = wibo::handles().getAs<ProcessObject>(hProcess);
		if (!obj) {
			setLastError(ERROR_INVALID_HANDLE);
			return 0;
		}
	}

	DWORD_PTR systemMask = computeSystemAffinityMask();
	if ((dwProcessAffinityMask & systemMask) == 0 || (dwProcessAffinityMask & ~systemMask) != 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	g_processAffinityMask = dwProcessAffinityMask & systemMask;
	g_processAffinityMaskInitialized = true;
	return TRUE;
}

DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetThreadAffinityMask(%p, 0x%lx)\n", hThread, static_cast<unsigned long>(dwThreadAffinityMask));
	if (dwThreadAffinityMask == 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	if (!isPseudoCurrentThreadHandle(hThread)) {
		auto obj = wibo::handles().getAs<ThreadObject>(hThread);
		if (!obj) {
			setLastError(ERROR_INVALID_HANDLE);
			return 0;
		}
	}

	DWORD_PTR processMask = 0;
	DWORD_PTR systemMask = 0;
	if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
		return 0;
	}
	if ((dwThreadAffinityMask & ~systemMask) != 0 || (dwThreadAffinityMask & processMask) == 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	return processMask;
}

[[noreturn]] void exitInternal(DWORD exitCode) {
	DEBUG_LOG("exitInternal(%u)\n", exitCode);
	wibo::handles().clear();
	// On macOS this also clears Rosetta's reserved Win64 GS/TSD slot. Leaving a
	// guest TEB installed while the host process exits can strand the translated
	// process in an uninterruptible exiting state.
	wibo::uninstallTebForCurrentThread();
	_exit(static_cast<int>(exitCode));
}

void WINAPI ExitProcess(UINT uExitCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ExitProcess(%u)\n", uExitCode);
	exitInternal(uExitCode);
}

BOOL WINAPI TerminateProcess(HANDLE hProcess, UINT uExitCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("TerminateProcess(%p, %u)\n", hProcess, uExitCode);
	if (isPseudoCurrentProcessHandle(hProcess)) {
		exitInternal(uExitCode);
	}
	HandleMeta meta{};
	auto process = wibo::handles().getAs<ProcessObject>(hProcess, &meta);
	if (!process) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!(meta.grantedAccess & PROCESS_TERMINATE)) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	if (process->pid == getpid())
		exitInternal(uExitCode);
	std::lock_guard lk(process->m);
	if (process->signaled) {
		return TRUE;
	}
	int killResult = 0;
#if defined(__linux__)
	if (process->pidfd != -1) {
		if (syscall(SYS_pidfd_send_signal, process->pidfd, SIGKILL, nullptr, 0) != 0) {
			killResult = errno;
			DEBUG_LOG("TerminateProcess: pidfd_send_signal(%d) failed: %s\n", process->pidfd, strerror(killResult));
		}
	} else if (kill(process->pid, SIGKILL) != 0) {
		killResult = errno;
		DEBUG_LOG("TerminateProcess: kill(%d) failed: %s\n", process->pid, strerror(killResult));
	}
#else
	if (kill(process->pid, SIGKILL) != 0) {
		killResult = errno;
		DEBUG_LOG("TerminateProcess: kill(%d) failed: %s\n", process->pid, strerror(killResult));
	}
#endif
	if (killResult != 0) {
		switch (killResult) {
		case ESRCH:
		case EPERM:
			setLastError(ERROR_ACCESS_DENIED);
			break;
		default:
			setLastError(ERROR_INVALID_PARAMETER);
			break;
		}
		return FALSE;
	}
	process->exitCode = uExitCode;
	process->forcedExitCode = true;
	process->exitCodeKnown = true;
	return TRUE;
}

BOOL WINAPI GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetExitCodeProcess(%p, %p)\n", hProcess, lpExitCode);
	if (!lpExitCode) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (isPseudoCurrentProcessHandle(hProcess)) {
		*lpExitCode = STILL_ACTIVE;
		return TRUE;
	}
	HandleMeta meta{};
	auto process = wibo::handles().getAs<ProcessObject>(hProcess, &meta);
	if (!process) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	DWORD exitCode = STILL_ACTIVE;
	if (!(meta.grantedAccess & (PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION))) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	std::lock_guard lk(process->m);
	if (process->signaled) {
		if (!process->exitCodeKnown) {
			setLastError(ERROR_NOT_SUPPORTED);
			return FALSE;
		}
		exitCode = process->exitCode;
	}
	*lpExitCode = exitCode;
	return TRUE;
}

DWORD WINAPI TlsAlloc() {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("TlsAlloc()\n");
	DWORD index = wibo::tls::reserveSlot();
	if (index == wibo::tls::kInvalidTlsIndex) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return TLS_OUT_OF_INDEXES;
	}
	setLastError(ERROR_SUCCESS);
	return index;
}

BOOL WINAPI TlsFree(DWORD dwTlsIndex) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("TlsFree(%u)\n", dwTlsIndex);
	if (!wibo::tls::releaseSlot(dwTlsIndex)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	setLastError(ERROR_SUCCESS);
	return TRUE;
}

LPVOID WINAPI TlsGetValue(DWORD dwTlsIndex) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("TlsGetValue(%u)\n", dwTlsIndex);
	if (!wibo::tls::isSlotAllocated(dwTlsIndex)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return nullptr;
	}
	GUEST_PTR result = wibo::tls::getValue(dwTlsIndex);
	setLastError(ERROR_SUCCESS);
	return reinterpret_cast<LPVOID>(result);
}

BOOL WINAPI TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("TlsSetValue(%u, %p)\n", dwTlsIndex, lpTlsValue);
	if (!wibo::tls::isSlotAllocated(dwTlsIndex)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!wibo::tls::setValue(dwTlsIndex, toGuestPtr(lpTlsValue))) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	setLastError(ERROR_SUCCESS);
	return TRUE;
}

HRESULT WINAPI SetThreadDescription(HANDLE hThread, LPCWSTR lpThreadDescription) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetThreadDescription(%p, %p)\n", hThread, lpThreadDescription);
	(void)hThread;
	(void)lpThreadDescription;
	return S_OK;
}

namespace {
Pin<ThreadObject> startThread(SIZE_T dwStackSize, std::function<DWORD()> entry, DWORD dwCreationFlags, DWORD &error) {
	error = ERROR_SUCCESS;
	constexpr DWORD CREATE_SUSPENDED = 0x00000004;
	constexpr DWORD STACK_SIZE_PARAM_IS_A_RESERVATION = 0x00010000;
	constexpr DWORD SUPPORTED_FLAGS = CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION;
	if ((dwCreationFlags & ~SUPPORTED_FLAGS) != 0) {
		DEBUG_LOG("CreateThread: unsupported creation flags 0x%x\n", dwCreationFlags);
		error = ERROR_NOT_SUPPORTED;
		return {};
	}

	Pin<ThreadObject> obj = make_pin<ThreadObject>(); // tid set during pthread_create
	if ((dwCreationFlags & CREATE_SUSPENDED) != 0) {
		obj->suspendCount = 1;
	}
	ThreadStartData *startData = new ThreadStartData{obj.get(), std::move(entry)};
	detail::ref(obj.get()); // Increment ref for the new thread to adopt

	pthread_attr_t attr;
	int rc = pthread_attr_init(&attr);
	if (rc != 0) {
		delete startData;
		detail::deref(obj.get());
		error = wibo::winErrorFromErrno(rc);
		return {};
	}
	size_t stackReserve = pthreadStackReserve(dwStackSize, dwCreationFlags);
	rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (rc == 0 && stackReserve != 0) {
		rc = pthread_attr_setstacksize(&attr, stackReserve);
	} else if (rc == 0) {
		rc = EINVAL;
	}
	if (rc != 0) {
		pthread_attr_destroy(&attr);
		delete startData;
		detail::deref(obj.get());
		error = wibo::winErrorFromErrno(rc);
		return {};
	}

	rc = pthread_create(&obj->thread, &attr, threadTrampoline, startData);
	pthread_attr_destroy(&attr);
	if (rc != 0) {
		// Clean up
		delete startData;
		detail::deref(obj.get());
		error = wibo::winErrorFromErrno(rc);
		return {};
	}

	{
		std::unique_lock lock(obj->m);
		obj->cv.wait(lock, [&] { return obj->initialized; });
	}
	return obj;
}

} // namespace

bool createWorkerThread(DWORD (*function)(void *), void *parameter, DWORD &error) {
	return static_cast<bool>(startThread(0, [=] { return function(parameter); }, 0, error));
}

HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
						   LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags,
						   LPDWORD lpThreadId) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateThread(%p, %zu, %p, %p, %u, %p)\n", lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter,
			  dwCreationFlags, lpThreadId);
	DWORD error = ERROR_SUCCESS;
	auto object = startThread(
		dwStackSize, [=] { return lpStartAddress ? call_LPTHREAD_START_ROUTINE(lpStartAddress, lpParameter) : 0; },
		dwCreationFlags, error);
	if (!object) {
		setLastError(error);
		return NO_HANDLE;
	}
	if (lpThreadId)
		*lpThreadId = object->threadId;
	return allocateThreadHandle(std::move(object), THREAD_ALL_ACCESS,
								lpThreadAttributes && lpThreadAttributes->bInheritHandle ? HANDLE_FLAG_INHERIT : 0);
}

[[noreturn]] void WINAPI ExitThread(DWORD dwExitCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ExitThread(%u)\n", dwExitCode);
	ThreadObject *obj = g_currentThreadObject;
	{
		std::lock_guard lk(obj->m);
		obj->exitCode = dwExitCode;
	}
	// Can't use pthread_cleanup_push/pop because it can't unwind the Windows stack
	// So call the cleanup function directly before pthread_exit
	threadCleanup(obj);
	pthread_exit(nullptr);
}

BOOL WINAPI GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetExitCodeThread(%p, %p)\n", hThread, lpExitCode);
	if (!lpExitCode) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (isPseudoCurrentThreadHandle(hThread)) {
		*lpExitCode = STILL_ACTIVE;
		return TRUE;
	}
	HandleMeta metadata{};
	auto obj = wibo::handles().getAs<ThreadObject>(hThread, &metadata);
	if (!obj) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!(metadata.grantedAccess & (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION))) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	std::lock_guard lk(obj->m);
	*lpExitCode = obj->signaled ? obj->exitCode : STILL_ACTIVE;
	return TRUE;
}

BOOL WINAPI SetThreadPriority(HANDLE hThread, int nPriority) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetThreadPriority(%p, %d)\n", hThread, nPriority);
	(void)hThread;
	(void)nPriority;
	return TRUE;
}

BOOL WINAPI SetThreadPriorityBoost(HANDLE hThread, BOOL bDisablePriorityBoost) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetThreadPriorityBoost(%p, %d)\n", hThread, bDisablePriorityBoost);
	(void)bDisablePriorityBoost;
	if (!isPseudoCurrentThreadHandle(hThread) && !wibo::handles().getAs<ThreadObject>(hThread)) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	setLastError(ERROR_SUCCESS);
	return TRUE;
}

DWORD WINAPI SetThreadIdealProcessor(HANDLE hThread, DWORD dwIdealProcessor) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetThreadIdealProcessor(%p, %u)\n", hThread, dwIdealProcessor);
	if (!isPseudoCurrentThreadHandle(hThread) && !wibo::handles().getAs<ThreadObject>(hThread)) {
		setLastError(ERROR_INVALID_HANDLE);
		return static_cast<DWORD>(-1);
	}

	long reported = sysconf(_SC_NPROCESSORS_ONLN);
	DWORD logicalCount = reported > 0 ? static_cast<DWORD>(reported) : 1;
	if (dwIdealProcessor >= logicalCount) {
		setLastError(ERROR_INVALID_PARAMETER);
		return static_cast<DWORD>(-1);
	}

	static thread_local DWORD currentIdealProcessor = static_cast<DWORD>(-1);
	DWORD previous = currentIdealProcessor;
	if (isPseudoCurrentThreadHandle(hThread)) {
		currentIdealProcessor = dwIdealProcessor;
	}
	setLastError(ERROR_SUCCESS);
	return previous;
}

int WINAPI GetThreadPriority(HANDLE hThread) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetThreadPriority(%p)\n", hThread);
	(void)hThread;
	return 0;
}

DWORD WINAPI GetPriorityClass(HANDLE hProcess) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetPriorityClass(%p)\n", hProcess);
	(void)hProcess;
	return NORMAL_PRIORITY_CLASS;
}

BOOL WINAPI GetThreadTimes(HANDLE hThread, FILETIME *lpCreationTime, FILETIME *lpExitTime, FILETIME *lpKernelTime,
						   FILETIME *lpUserTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetThreadTimes(%p, %p, %p, %p, %p)\n", hThread, lpCreationTime, lpExitTime, lpKernelTime, lpUserTime);

	if (!lpKernelTime || !lpUserTime) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (!isPseudoCurrentThreadHandle(hThread)) {
		DEBUG_LOG("GetThreadTimes: unsupported handle %p\n", hThread);
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	if (lpCreationTime) {
		*lpCreationTime = kDefaultThreadFileTime;
	}
	if (lpExitTime) {
		lpExitTime->dwLowDateTime = 0;
		lpExitTime->dwHighDateTime = 0;
	}

#ifdef __linux__
	struct rusage usage {};
	if (getrusage(RUSAGE_THREAD, &usage) == 0) {
		*lpKernelTime = fileTimeFromTimeval(usage.ru_stime);
		*lpUserTime = fileTimeFromTimeval(usage.ru_utime);
		return TRUE;
	}
#endif

	struct timespec cpuTime {};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpuTime) == 0) {
		*lpKernelTime = fileTimeFromDuration(0);
		*lpUserTime = fileTimeFromTimespec(cpuTime);
		return TRUE;
	}

	kernel32::setLastErrorFromErrno();
	*lpKernelTime = fileTimeFromDuration(0);
	*lpUserTime = fileTimeFromDuration(0);
	return FALSE;
}

BOOL WINAPI CreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes,
						   LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags,
						   LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo,
						   LPPROCESS_INFORMATION lpProcessInformation) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateProcessA %s \"%s\" %p %p %d 0x%x %p %s %p %p\n", lpApplicationName ? lpApplicationName : "<null>",
			  lpCommandLine ? lpCommandLine : "<null>", lpProcessAttributes, lpThreadAttributes, bInheritHandles,
			  dwCreationFlags, lpEnvironment, lpCurrentDirectory ? lpCurrentDirectory : "<none>", lpStartupInfo,
			  lpProcessInformation);

	bool useSearchPath = lpApplicationName == nullptr;
	if (dwCreationFlags & EXTENDED_STARTUPINFO_PRESENT) {
		if (!lpStartupInfo || lpStartupInfo->cb != sizeof(STARTUPINFOEXA)) {
			setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		auto *extended = reinterpret_cast<STARTUPINFOEXA *>(lpStartupInfo);
		if (extended->lpAttributeList) {
			auto *list = reinterpret_cast<ProcessAttributeList *>(static_cast<uintptr_t>(extended->lpAttributeList));
			if (list->count) {
				// Guest handles cannot yet be transferred to an exec-created process.
				setLastError(ERROR_NOT_SUPPORTED);
				return FALSE;
			}
		}
	}
	std::string application;
	std::string commandLine = lpCommandLine ? lpCommandLine : "";
	if (lpApplicationName) {
		application = lpApplicationName;
	} else {
		std::vector<std::string> arguments = wibo::splitCommandLine(commandLine.c_str());
		if (arguments.empty()) {
			setLastError(ERROR_FILE_NOT_FOUND);
			return FALSE;
		}
		application = arguments.front();
	}

	auto resolved = wibo::resolveExecutable(application, useSearchPath);
	if (!resolved) {
		setLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}

	Pin<ProcessObject> obj;
	int spawnResult = wibo::spawnWithCommandLine(*resolved, commandLine, obj);
	if (spawnResult != 0) {
		setLastError((spawnResult == ENOENT) ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
		return FALSE;
	}

	if (lpProcessInformation) {
		lpProcessInformation->dwProcessId = static_cast<DWORD>(obj->pid);
		lpProcessInformation->dwThreadId = static_cast<DWORD>(obj->pid); // Use the process ID as the thread ID
		lpProcessInformation->hProcess = wibo::handles().alloc(obj.clone(), PROCESS_ALL_ACCESS, 0);
		// Give hThread a process handle for now
		lpProcessInformation->hThread = wibo::handles().alloc(std::move(obj), PROCESS_ALL_ACCESS, 0);
	}
	(void)lpProcessAttributes;
	(void)lpThreadAttributes;
	(void)bInheritHandles;
	(void)dwCreationFlags;
	(void)lpEnvironment;
	(void)lpCurrentDirectory;
	(void)lpStartupInfo;
	return TRUE;
}

BOOL WINAPI CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes,
						   LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags,
						   LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
						   LPPROCESS_INFORMATION lpProcessInformation) {
	HOST_CONTEXT_GUARD();
	std::string applicationUtf8;
	if (lpApplicationName) {
		applicationUtf8 = wideStringToString(lpApplicationName);
	}
	std::string commandUtf8;
	if (lpCommandLine) {
		commandUtf8 = wideStringToString(lpCommandLine);
	}
	std::string directoryUtf8;
	if (lpCurrentDirectory) {
		directoryUtf8 = wideStringToString(lpCurrentDirectory);
	}
	DEBUG_LOG("CreateProcessW %s \"%s\" %p %p %d 0x%x %p %s %p %p\n",
			  applicationUtf8.empty() ? "<null>" : applicationUtf8.c_str(),
			  commandUtf8.empty() ? "<null>" : commandUtf8.c_str(), lpProcessAttributes, lpThreadAttributes,
			  bInheritHandles, dwCreationFlags, lpEnvironment, directoryUtf8.empty() ? "<none>" : directoryUtf8.c_str(),
			  lpStartupInfo, lpProcessInformation);
	std::vector<char> commandBuffer;
	if (!commandUtf8.empty()) {
		commandBuffer.assign(commandUtf8.begin(), commandUtf8.end());
		commandBuffer.push_back('\0');
	}
	LPSTR commandPtr = commandBuffer.empty() ? nullptr : commandBuffer.data();
	LPCSTR applicationPtr = applicationUtf8.empty() ? nullptr : applicationUtf8.c_str();
	LPCSTR directoryPtr = directoryUtf8.empty() ? nullptr : directoryUtf8.c_str();
	return CreateProcessA(applicationPtr, commandPtr, lpProcessAttributes, lpThreadAttributes, bInheritHandles,
						  dwCreationFlags, lpEnvironment, directoryPtr, reinterpret_cast<LPSTARTUPINFOA>(lpStartupInfo),
						  lpProcessInformation);
}

void WINAPI GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetStartupInfoA(%p)\n", lpStartupInfo);
	populateStartupInfo(lpStartupInfo);
}

void WINAPI GetStartupInfoW(LPSTARTUPINFOW lpStartupInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetStartupInfoW(%p)\n", lpStartupInfo);
	populateStartupInfo(lpStartupInfo);
}

BOOL WINAPI SetThreadStackGuarantee(PULONG StackSizeInBytes) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetThreadStackGuarantee(%p)\n", StackSizeInBytes);
	(void)StackSizeInBytes;
	return TRUE;
}

} // namespace kernel32
