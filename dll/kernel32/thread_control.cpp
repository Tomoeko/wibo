#include "processthreadsapi.h"

#include "context.h"
#include "errors.h"
#include "internal.h"

#ifdef __APPLE__
#include <mach/i386/thread_status.h>
#include <mach/mach.h>
#include <pthread.h>
#endif

#include <cstddef>
#include <cstring>
#include <sched.h>

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

BOOL WINAPI SwitchToThread() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SwitchToThread()\n");
	// The host reports whether it accepted the yield, not whether another thread ran.
	return sched_yield() == 0;
}

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

static_assert(sizeof(CONTEXT64) == 1232);
static_assert(offsetof(CONTEXT64, ContextFlags) == 48);
static_assert(offsetof(CONTEXT64, Rip) == 248);
static_assert(offsetof(CONTEXT64, FltSave) == 256);
static_assert(sizeof(XMM_SAVE_AREA32) == 512);
static_assert(sizeof(CONTEXT32) == 716);

namespace kernel32 {
void captureThreadSelectors(ThreadObject &thread) {
	asm volatile("mov %%cs, %0; mov %%ss, %1" : "=r"(thread.entryCs), "=r"(thread.entrySs));
}

BOOL WINAPI GetThreadContext(HANDLE hThread, LPCONTEXT context) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetThreadContext(%p, %p)\n", hThread, context);
	if (!context) {
		setLastError(ERROR_NOACCESS);
		return FALSE;
	}
	HandleMeta metadata{};
	auto thread = isPseudoCurrentThreadHandle(hThread) ? currentThreadObject()
													   : wibo::handles().getAs<ThreadObject>(hThread, &metadata);
	if (!thread) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	constexpr DWORD kGetContextAccess = 8;
	if (!isPseudoCurrentThreadHandle(hThread) && !(metadata.grantedAccess & kGetContextAccess)) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
#if defined(__APPLE__) && defined(WIBO_GUEST_64)
	if (reinterpret_cast<uintptr_t>(context) % alignof(CONTEXT)) {
		setLastError(ERROR_NOACCESS);
		return FALSE;
	}
	const DWORD flags = context->ContextFlags;
	constexpr DWORD kSupported = 1 | 2 | 8;
	if ((flags & ~kSupported) != CONTEXT_ARCH) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	std::lock_guard lock(thread->m);
	if (thread->signaled || !thread->hostSuspended || pthread_equal(thread->thread, pthread_self())) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	const thread_t port = pthread_mach_thread_np(thread->thread);
	x86_thread_state64_t state{};
	mach_msg_type_number_t stateCount = x86_THREAD_STATE64_COUNT;
	kern_return_t result =
		thread_get_state(port, x86_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &stateCount);
	if (result != KERN_SUCCESS || stateCount != x86_THREAD_STATE64_COUNT) {
		setLastError(result == KERN_SUCCESS ? ERROR_INVALID_PARAMETER : controlError(result));
		return FALSE;
	}
	CONTEXT output;
	// Preserve the byte representation of unrequested register groups.
	std::memcpy(&output, context, sizeof(output));
	if (flags & 1) {
		x86_thread_full_state64_t full{};
		mach_msg_type_number_t fullCount = x86_THREAD_FULL_STATE64_COUNT;
		const auto fullResult =
			thread_get_state(port, x86_THREAD_FULL_STATE64, reinterpret_cast<thread_state_t>(&full), &fullCount);
		if (fullResult == KERN_SUCCESS && fullCount == x86_THREAD_FULL_STATE64_COUNT)
			output.SegSs = static_cast<WORD>(full.__ss);
		else {
			// The flat long-mode stack selector is stable across supported guest transitions.
			if (state.__cs != thread->entryCs) {
				setLastError(ERROR_NOT_SUPPORTED);
				return FALSE;
			}
			output.SegSs = thread->entrySs;
		}
		output.Rsp = state.__rsp;
		output.Rip = state.__rip;
		output.SegCs = static_cast<WORD>(state.__cs);
		output.EFlags = static_cast<DWORD>(state.__rflags);
	}
	if (flags & 2) {
		output.Rax = state.__rax;
		output.Rcx = state.__rcx;
		output.Rdx = state.__rdx;
		output.Rbx = state.__rbx;
		output.Rbp = state.__rbp;
		output.Rsi = state.__rsi;
		output.Rdi = state.__rdi;
		output.R8 = state.__r8;
		output.R9 = state.__r9;
		output.R10 = state.__r10;
		output.R11 = state.__r11;
		output.R12 = state.__r12;
		output.R13 = state.__r13;
		output.R14 = state.__r14;
		output.R15 = state.__r15;
	}
	if (flags & 8) {
		static_assert(offsetof(x86_float_state64_t, __fpu_xmm0) - offsetof(x86_float_state64_t, __fpu_fcw) == 160);
		static_assert(offsetof(x86_float_state64_t, __fpu_mxcsr) - offsetof(x86_float_state64_t, __fpu_fcw) == 24);
		x86_float_state64_t floating{};
		mach_msg_type_number_t floatCount = x86_FLOAT_STATE64_COUNT;
		result = thread_get_state(port, x86_FLOAT_STATE64, reinterpret_cast<thread_state_t>(&floating), &floatCount);
		if (result != KERN_SUCCESS || floatCount != x86_FLOAT_STATE64_COUNT) {
			setLastError(result == KERN_SUCCESS ? ERROR_INVALID_PARAMETER : controlError(result));
			return FALSE;
		}
		std::memcpy(&output.FltSave,
					reinterpret_cast<const uint8_t *>(&floating) + offsetof(x86_float_state64_t, __fpu_fcw),
					sizeof(output.FltSave));
		output.MxCsr = floating.__fpu_mxcsr;
	}
	*context = output;
	return TRUE;
#else
	setLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
#endif
}
} // namespace kernel32
