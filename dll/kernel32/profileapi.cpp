#include "profileapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "processthreadsapi.h"

#include <chrono>

namespace {

constexpr LONGLONG kPerformanceCounterFrequency = 1000000000LL;

} // namespace

namespace kernel32 {

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("QueryPerformanceCounter(%p)\n", lpPerformanceCount);
	if (!lpPerformanceCount) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
	lpPerformanceCount->QuadPart = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
	return TRUE;
}

BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("QueryPerformanceFrequency(%p)\n", lpFrequency);
	if (!lpFrequency) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	lpFrequency->QuadPart = kPerformanceCounterFrequency;
	return TRUE;
}

BOOL WINAPI QueryThreadCycleTime(HANDLE thread, ULONGLONG *cycles) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("QueryThreadCycleTime(%p, %p)\n", thread, cycles);
	if (!cycles) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!isPseudoCurrentThreadHandle(thread)) {
		HandleMeta metadata{};
		auto object = wibo::handles().get(thread, &metadata);
		if (!object || (object->type != ObjectType::Thread && object->type != ObjectType::ProcessThread)) {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
		if (!(metadata.grantedAccess & (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION))) {
			setLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}
	// Host thread accounting does not provide an exact CPU cycle count.
	setLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

} // namespace kernel32
