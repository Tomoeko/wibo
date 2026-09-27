#include "processthreadsapi.h"

#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"

#include <climits>
#include <mutex>
#include <utility>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#else
#include <sched.h>
#endif

namespace {

using kernel32::ThreadObject;

Pin<ThreadObject> priorityThread(HANDLE handle, DWORD requiredAccess, DWORD &error) {
	if (kernel32::isPseudoCurrentThreadHandle(handle)) {
		auto thread = kernel32::currentThreadObject();
		error = thread ? ERROR_SUCCESS : ERROR_NOT_SUPPORTED;
		return thread;
	}
	HandleMeta metadata{};
	auto object = wibo::handles().get(handle, &metadata);
	if (!object || (object->type != ObjectType::Thread && object->type != ObjectType::ProcessThread)) {
		error = ERROR_INVALID_HANDLE;
		return {};
	}
	if (!(metadata.grantedAccess & requiredAccess)) {
		error = ERROR_ACCESS_DENIED;
		return {};
	}
	if (object->type == ObjectType::ProcessThread) {
		error = ERROR_NOT_SUPPORTED;
		return {};
	}
	error = ERROR_SUCCESS;
	return std::move(object).downcast<ThreadObject>();
}

bool applyHostPriority(const ThreadObject &thread, int priority) {
#ifdef __APPLE__
	const thread_t port = pthread_mach_thread_np(thread.thread);
	if (!MACH_PORT_VALID(port))
		return false;
	thread_precedence_policy_data_t policy{priority};
	const kern_return_t applied = thread_policy_set(
		port, THREAD_PRECEDENCE_POLICY, reinterpret_cast<thread_policy_t>(&policy), THREAD_PRECEDENCE_POLICY_COUNT);
	if (applied != KERN_SUCCESS)
		return false;
	thread_precedence_policy_data_t observed{};
	mach_msg_type_number_t count = THREAD_PRECEDENCE_POLICY_COUNT;
	boolean_t defaultPolicy = FALSE;
	const kern_return_t queried = thread_policy_get(
		port, THREAD_PRECEDENCE_POLICY, reinterpret_cast<thread_policy_t>(&observed), &count, &defaultPolicy);
	return queried == KERN_SUCCESS && count == THREAD_PRECEDENCE_POLICY_COUNT && !defaultPolicy &&
		   observed.importance == priority;
#else
	// SCHED_OTHER exposes only priority zero. Other levels need a host scheduler
	// operation with a verified effect before they can be reported as applied.
	if (priority != 0)
		return false;
	int policy = 0;
	sched_param parameters{};
	return pthread_getschedparam(thread.thread, &policy, &parameters) == 0 && policy == SCHED_OTHER &&
		   parameters.sched_priority == 0;
#endif
}

} // namespace

namespace kernel32 {

BOOL WINAPI SetThreadPriority(HANDLE handle, int priority) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetThreadPriority(%p, %d)\n", handle, priority);
	DWORD error = ERROR_SUCCESS;
	auto thread = priorityThread(handle, THREAD_SET_INFORMATION, error);
	if (!thread) {
		setLastError(error);
		return FALSE;
	}
	if (priority < -2 || priority > 2) {
		const bool documentedMode = priority == -15 || priority == 15 || priority == 0x10000 || priority == 0x20000;
		setLastError(documentedMode ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	std::lock_guard lock(thread->m);
	if (!thread->signaled) {
		if (!thread->initialized || !applyHostPriority(*thread, priority)) {
			setLastError(ERROR_NOT_SUPPORTED);
			return FALSE;
		}
	}
	thread->priority = priority;
	return TRUE;
}

int WINAPI GetThreadPriority(HANDLE handle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetThreadPriority(%p)\n", handle);
	DWORD error = ERROR_SUCCESS;
	auto thread = priorityThread(handle, THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, error);
	if (!thread) {
		setLastError(error);
		return INT_MAX;
	}
	std::lock_guard lock(thread->m);
	return thread->priority;
}

} // namespace kernel32
