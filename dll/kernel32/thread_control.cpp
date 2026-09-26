#include "processthreadsapi.h"

#include "context.h"
#include "errors.h"
#include "internal.h"

#ifdef __APPLE__
#include <mach/mach.h>
#include <pthread.h>
#endif

namespace kernel32 {
namespace {
constexpr DWORD kFailure = static_cast<DWORD>(-1);
constexpr unsigned kMaximumSuspendCount = 127;
#ifdef __APPLE__
DWORD controlError(kern_return_t result) {
	return result == KERN_INVALID_ARGUMENT ? ERROR_INVALID_HANDLE : ERROR_GEN_FAILURE;
}
#endif
} // namespace

DWORD WINAPI SuspendThread(HANDLE hThread) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SuspendThread(%p)\n", hThread);
	HandleMeta metadata{};
	auto object = isPseudoCurrentThreadHandle(hThread) ? currentThreadObject()
													   : wibo::handles().getAs<ThreadObject>(hThread, &metadata);
	if (!object) {
		setLastError(ERROR_INVALID_HANDLE);
		return kFailure;
	}
	if (!isPseudoCurrentThreadHandle(hThread) && !(metadata.grantedAccess & THREAD_SUSPEND_RESUME)) {
		setLastError(ERROR_ACCESS_DENIED);
		return kFailure;
	}
	std::lock_guard lock(object->m);
	if (object->signaled) {
		setLastError(ERROR_ACCESS_DENIED);
		return kFailure;
	}
	if (pthread_equal(object->thread, pthread_self())) {
		// Self-suspension needs a handoff that does not retain the control mutex.
		setLastError(ERROR_NOT_SUPPORTED);
		return kFailure;
	}
	const DWORD previous = object->suspendCount;
	if (previous == kMaximumSuspendCount) {
		setLastError(ERROR_SIGNAL_REFUSED);
		return kFailure;
	}
	if (!previous) {
#ifdef __APPLE__
		const kern_return_t result = thread_suspend(pthread_mach_thread_np(object->thread));
		if (result != KERN_SUCCESS) {
			setLastError(controlError(result));
			return kFailure;
		}
		object->hostSuspended = true;
#else
		setLastError(ERROR_NOT_SUPPORTED);
		return kFailure;
#endif
	}
	++object->suspendCount;
	return previous;
}

DWORD WINAPI ResumeThread(HANDLE hThread) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ResumeThread(%p)\n", hThread);
	HandleMeta metadata{};
	auto object = isPseudoCurrentThreadHandle(hThread) ? currentThreadObject()
													   : wibo::handles().getAs<ThreadObject>(hThread, &metadata);
	if (!object) {
		setLastError(ERROR_INVALID_HANDLE);
		return kFailure;
	}
	if (!isPseudoCurrentThreadHandle(hThread) && !(metadata.grantedAccess & THREAD_SUSPEND_RESUME)) {
		setLastError(ERROR_ACCESS_DENIED);
		return kFailure;
	}
	DWORD previous;
	{
		std::lock_guard lock(object->m);
		if (object->signaled) {
			setLastError(ERROR_ACCESS_DENIED);
			return kFailure;
		}
		previous = object->suspendCount;
		if (previous == 1 && object->hostSuspended) {
#ifdef __APPLE__
			const kern_return_t result = thread_resume(pthread_mach_thread_np(object->thread));
			if (result != KERN_SUCCESS) {
				setLastError(controlError(result));
				return kFailure;
			}
			object->hostSuspended = false;
#else
			setLastError(ERROR_NOT_SUPPORTED);
			return kFailure;
#endif
		}
		if (previous)
			--object->suspendCount;
	}
	if (previous == 1)
		object->cv.notify_all();
	return previous;
}
} // namespace kernel32
