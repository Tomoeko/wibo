#include "processthreadsapi.h"
#include "sysinfoapi.h"

#include "context.h"
#include "errors.h"
#include "internal.h"

#include <unistd.h>

namespace kernel32 {
namespace {
constexpr DWORD kFailure = static_cast<DWORD>(-1);
constexpr DWORD kMaximumProcessors = sizeof(DWORD_PTR) * 8;

DWORD threadAccessError(HANDLE thread, DWORD requiredAccess) {
	if (isPseudoCurrentThreadHandle(thread))
		return currentThreadObject() ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
	HandleMeta metadata{};
	auto object = wibo::handles().get(thread, &metadata);
	if (!object || (object->type != ObjectType::Thread && object->type != ObjectType::ProcessThread))
		return ERROR_INVALID_HANDLE;
	if (requiredAccess == THREAD_QUERY_LIMITED_INFORMATION)
		requiredAccess |= THREAD_QUERY_INFORMATION;
	return metadata.grantedAccess & requiredAccess ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

DWORD processorNumberError(DWORD number) {
	const long online = sysconf(_SC_NPROCESSORS_ONLN);
	if (online <= 0 || online > static_cast<long>(kMaximumProcessors))
		return ERROR_NOT_SUPPORTED;
	return number < static_cast<DWORD>(online) ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}
} // namespace

BOOL WINAPI GetProcessGroupAffinity(HANDLE process, USHORT *count, USHORT *groups) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetProcessGroupAffinity(%p, %p, %p)\n", process, count, groups);
	if (!count) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!isPseudoCurrentProcessHandle(process)) {
		HandleMeta metadata{};
		auto object = wibo::handles().getAs<ProcessObject>(process, &metadata);
		if (!object) {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
		if (!(metadata.grantedAccess & (PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION))) {
			setLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
		if (object->pid != getpid()) {
			setLastError(ERROR_NOT_SUPPORTED);
			return FALSE;
		}
	}
	if (*count == 0) {
		*count = 1;
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	if (!groups) {
		setLastError(ERROR_NOACCESS);
		return FALSE;
	}
	// All supported thread affinities belong to the process's single exposed group.
	groups[0] = 0;
	*count = 1;
	return TRUE;
}

BOOL WINAPI GetThreadGroupAffinity(HANDLE thread, GROUP_AFFINITY *affinity) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetThreadGroupAffinity(%p, %p)\n", thread, affinity);
	if (!affinity) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const DWORD error = threadAccessError(thread, THREAD_QUERY_LIMITED_INFORMATION);
	if (error != ERROR_SUCCESS) {
		setLastError(error);
		return FALSE;
	}
	if (!isPseudoCurrentThreadHandle(thread) && wibo::handles().getAs<ProcessThreadObject>(thread)) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	DWORD_PTR processMask = 0, systemMask = 0;
	if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
		return FALSE;
	*affinity = {processMask, 0, {0, 0, 0}};
	return TRUE;
}

BOOL WINAPI SetThreadGroupAffinity(HANDLE thread, const GROUP_AFFINITY *affinity, GROUP_AFFINITY *previous) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetThreadGroupAffinity(%p, %p, %p)\n", thread, affinity, previous);
	if (!affinity) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const DWORD error = threadAccessError(thread, THREAD_SET_INFORMATION);
	if (error != ERROR_SUCCESS) {
		setLastError(error);
		return FALSE;
	}
	if (!isPseudoCurrentThreadHandle(thread) && wibo::handles().getAs<ProcessThreadObject>(thread)) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	DWORD_PTR processMask = 0, systemMask = 0;
	if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
		return FALSE;
	if (affinity->Group != 0 || affinity->Reserved[0] != 0 || affinity->Reserved[1] != 0 ||
		affinity->Reserved[2] != 0 || affinity->Mask == 0 || (affinity->Mask & ~processMask) != 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (affinity->Mask != processMask) {
		// A narrower mask requires host scheduling control that is unavailable here.
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	if (previous)
		*previous = {processMask, 0, {0, 0, 0}};
	return TRUE;
}

BOOL WINAPI GetThreadIdealProcessorEx(HANDLE thread, PPROCESSOR_NUMBER ideal) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetThreadIdealProcessorEx(%p, %p)\n", thread, ideal);
	DWORD error = ideal ? threadAccessError(thread, THREAD_QUERY_LIMITED_INFORMATION) : ERROR_INVALID_PARAMETER;
	// A current-CPU sample and a cache-affinity tag do not establish a preferred logical CPU.
	setLastError(error == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : error);
	return FALSE;
}

BOOL WINAPI SetThreadIdealProcessorEx(HANDLE thread, PPROCESSOR_NUMBER ideal, PPROCESSOR_NUMBER previous) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetThreadIdealProcessorEx(%p, %p, %p)\n", thread, ideal, previous);
	DWORD error = ideal ? threadAccessError(thread, THREAD_SET_INFORMATION) : ERROR_INVALID_PARAMETER;
	if (error == ERROR_SUCCESS) {
		const PROCESSOR_NUMBER requested = *ideal;
		if (requested.Group != 0 || requested.Reserved != 0)
			error = ERROR_NOT_SUPPORTED;
		else
			error = processorNumberError(requested.Number);
	}
	// The host has no equivalent scheduler hint; do not publish an unaccepted preference.
	setLastError(error == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : error);
	return FALSE;
}

DWORD WINAPI SetThreadIdealProcessor(HANDLE hThread, DWORD dwIdealProcessor) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetThreadIdealProcessor(%p, %u)\n", hThread, dwIdealProcessor);
	DWORD error = threadAccessError(hThread, THREAD_SET_INFORMATION);
	if (error == ERROR_SUCCESS && dwIdealProcessor != kMaximumProcessors)
		error =
			dwIdealProcessor >= kMaximumProcessors ? ERROR_INVALID_PARAMETER : processorNumberError(dwIdealProcessor);
	// MAXIMUM_PROCESSORS queries the same unavailable preference without modifying it.
	setLastError(error == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : error);
	return kFailure;
}

} // namespace kernel32
