#include "processthreadsapi.h"

#include "context.h"
#include "errors.h"
#include "internal.h"

#ifdef __APPLE__
#include <mach/i386/thread_status.h>
#include <mach/mach.h>
#include <pthread.h>
#endif

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>

namespace kernel32 {
namespace {
constexpr DWORD kFailure = static_cast<DWORD>(-1);
constexpr unsigned kMaximumSuspendCount = 127;
std::mutex g_processThreadRegistryMutex;
std::unordered_map<DWORD, Pin<ProcessThreadObject>> g_processThreads;

void retireProcessThread(ProcessThreadObject *object) {
	Pin<ProcessThreadObject> retired;
	{
		std::lock_guard registryLock(g_processThreadRegistryMutex);
		std::lock_guard objectLock(object->m);
		if (!object->signaled || object->handleCount.load(std::memory_order_relaxed))
			return;
		auto found = g_processThreads.find(object->threadId);
		if (found != g_processThreads.end() && found->second.get() == object) {
			retired = std::move(found->second);
			g_processThreads.erase(found);
		}
	}
}
#ifdef __APPLE__
DWORD controlError(kern_return_t result) {
	return result == KERN_INVALID_ARGUMENT ? ERROR_INVALID_HANDLE : ERROR_GEN_FAILURE;
}
#endif
} // namespace

ProcessThreadObject::ProcessThreadObject(DWORD threadId, int resumeFd, bool suspended)
	: WaitableObject(kType), threadId(threadId), suspendCount(suspended ? 1 : 0), mResumeFd(resumeFd) {
	if (!suspended && mResumeFd >= 0) {
		close(mResumeFd);
		mResumeFd = -1;
	}
#ifdef __APPLE__
	if (mResumeFd >= 0) {
		int enabled = 1;
		if (setsockopt(mResumeFd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
			mResumeError = errno;
	}
#endif
	std::lock_guard lock(g_processThreadRegistryMutex);
	g_processThreads.insert_or_assign(threadId, Pin<ProcessThreadObject>::acquire(this));
}

ProcessThreadObject::~ProcessThreadObject() {
	if (mResumeFd >= 0)
		close(mResumeFd);
}

void ProcessThreadObject::onLastHandleClosed() noexcept { retireProcessThread(this); }

DWORD ProcessThreadObject::resumeInitial() {
	std::lock_guard lock(m);
	if (signaled) {
		setLastError(ERROR_ACCESS_DENIED);
		return kFailure;
	}
	const DWORD previous = suspendCount;
	if (!previous)
		return 0;
	if (mResumeError || mResumeFd < 0) {
		setLastError(mResumeError ? wibo::winErrorFromErrno(mResumeError) : ERROR_INVALID_HANDLE);
		return kFailure;
	}
	const char resume = 'R';
	int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
	flags |= MSG_NOSIGNAL;
#endif
	ssize_t sent;
	do {
		sent = send(mResumeFd, &resume, sizeof(resume), flags);
	} while (sent < 0 && errno == EINTR);
	if (sent != static_cast<ssize_t>(sizeof(resume))) {
		setLastError(sent < 0 ? wibo::winErrorFromErrno(errno) : ERROR_GEN_FAILURE);
		return kFailure;
	}
	suspendCount = 0;
	close(mResumeFd);
	mResumeFd = -1;
	return previous;
}

void ProcessThreadObject::complete(DWORD code, bool known) {
	{
		std::lock_guard lock(m);
		if (signaled)
			return;
		exitCode = code;
		exitCodeKnown = known;
		signaled = true;
		if (mResumeFd >= 0) {
			close(mResumeFd);
			mResumeFd = -1;
		}
	}
	cv.notify_all();
	notifyWaiters(false);
	retireProcessThread(this);
}

HANDLE allocateProcessThreadHandle(Pin<ProcessThreadObject> thread, DWORD access, DWORD flags) {
	std::lock_guard lock(g_processThreadRegistryMutex);
	// A newer thread may already own a recycled native ID.
	g_processThreads.try_emplace(thread->threadId, thread.clone());
	return wibo::handles().alloc(std::move(thread), access, flags);
}

HANDLE openProcessThreadHandle(DWORD threadId, DWORD access, DWORD flags) {
	std::lock_guard lock(g_processThreadRegistryMutex);
	auto found = g_processThreads.find(threadId);
	if (found == g_processThreads.end()) {
		setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}
	return wibo::handles().alloc(found->second.clone(), access, flags);
}

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
	if (!isPseudoCurrentThreadHandle(hThread)) {
		auto processThread = wibo::handles().getAs<ProcessThreadObject>(hThread, &metadata);
		if (processThread) {
			setLastError(metadata.grantedAccess & THREAD_SUSPEND_RESUME ? ERROR_NOT_SUPPORTED : ERROR_ACCESS_DENIED);
			return kFailure;
		}
	}
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
	if (!isPseudoCurrentThreadHandle(hThread)) {
		auto processThread = wibo::handles().getAs<ProcessThreadObject>(hThread, &metadata);
		if (processThread) {
			if (!(metadata.grantedAccess & THREAD_SUSPEND_RESUME)) {
				setLastError(ERROR_ACCESS_DENIED);
				return kFailure;
			}
			return processThread->resumeInitial();
		}
	}
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
	if (!isPseudoCurrentThreadHandle(hThread)) {
		auto processThread = wibo::handles().getAs<ProcessThreadObject>(hThread, &metadata);
		if (processThread) {
			constexpr DWORD kGetContextAccess = 8;
			setLastError(metadata.grantedAccess & kGetContextAccess ? ERROR_NOT_SUPPORTED : ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}
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
