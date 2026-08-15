#include "profileapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"

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

} // namespace kernel32
