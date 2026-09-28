#include "synchapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "kernel32.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>

namespace {

constexpr ULONG kExecuteInIoThread = 0x01;
constexpr ULONG kExecuteInWaitThread = 0x04;
constexpr ULONG kExecuteOnlyOnce = 0x08;
constexpr ULONG kExecuteLongFunction = 0x10;
constexpr ULONG kSupportedFlags = kExecuteInIoThread | kExecuteInWaitThread | kExecuteOnlyOnce | kExecuteLongFunction;
constexpr size_t kMaxPendingCallbacks = 512;

struct RegisteredWait {
	std::mutex mutex;
	std::condition_variable cv;
	Pin<kernel32::EventObject> stop;
	Pin<kernel32::EventObject> completion;
	HANDLE targetHandle = NO_HANDLE;
	HANDLE stopHandle = NO_HANDLE;
	WAITORTIMERCALLBACK callback = nullptr;
	PVOID context = nullptr;
	ULONG milliseconds = INFINITE;
	ULONG flags = 0;
	size_t pendingCallbacks = 0;
	bool cancelled = false;
	bool waitDone = false;

	~RegisteredWait() {
		if (targetHandle != NO_HANDLE)
			wibo::handles().release(targetHandle);
		if (stopHandle != NO_HANDLE)
			wibo::handles().release(stopHandle);
	}

	void callbackDone() {
		Pin<kernel32::EventObject> completed;
		{
			std::lock_guard lock(mutex);
			--pendingCallbacks;
			if (cancelled && waitDone && !pendingCallbacks)
				completed = completion.clone();
		}
		cv.notify_all();
		if (completed)
			completed->set();
	}

	void finishWait() {
		Pin<kernel32::EventObject> completed;
		{
			std::lock_guard lock(mutex);
			waitDone = true;
			if (cancelled && !pendingCallbacks)
				completed = completion.clone();
		}
		cv.notify_all();
		if (completed)
			completed->set();
	}
};

struct CallbackTask {
	std::shared_ptr<RegisteredWait> wait;
	BOOLEAN timedOut;
};

std::mutex g_waitsMutex;
std::unordered_map<HANDLE, std::shared_ptr<RegisteredWait>> g_waits;
uint32_t g_nextWaitHandle = 0x40000000;
thread_local RegisteredWait *g_activeCallback = nullptr;

DWORD invokeCallback(void *parameter) {
	std::unique_ptr<CallbackTask> task(static_cast<CallbackTask *>(parameter));
	auto wait = task->wait;
	auto *previous = g_activeCallback;
	g_activeCallback = wait.get();
	call_WAITORTIMERCALLBACK(wait->callback, wait->context, task->timedOut);
	g_activeCallback = previous;
	wait->callbackDone();
	return 0;
}

void queueCallback(const std::shared_ptr<RegisteredWait> &wait, BOOLEAN timedOut) {
	{
		std::unique_lock lock(wait->mutex);
		wait->cv.wait(lock, [&] { return wait->cancelled || wait->pendingCallbacks < kMaxPendingCallbacks; });
		if (wait->cancelled)
			return;
		++wait->pendingCallbacks;
	}
	if (wait->flags & kExecuteInWaitThread) {
		CallbackTask task{wait, timedOut};
		auto *previous = g_activeCallback;
		g_activeCallback = wait.get();
		call_WAITORTIMERCALLBACK(wait->callback, wait->context, task.timedOut);
		g_activeCallback = previous;
		wait->callbackDone();
		return;
	}
	auto *task = new (std::nothrow) CallbackTask{wait, timedOut};
	DWORD error = ERROR_NOT_ENOUGH_MEMORY;
	if (!task || !kernel32::createWorkerThread(&invokeCallback, task, error)) {
		delete task;
		{
			std::lock_guard lock(wait->mutex);
			wait->cancelled = true;
		}
		wait->stop->set();
		wait->callbackDone();
		DEBUG_LOG("Registered wait callback thread failed: %u\n", error);
	}
}

DWORD runWait(void *parameter) {
	std::unique_ptr<std::shared_ptr<RegisteredWait>> holder(static_cast<std::shared_ptr<RegisteredWait> *>(parameter));
	const auto &wait = *holder;
	const HANDLE handles[] = {wait->stopHandle, wait->targetHandle};
	for (;;) {
		const DWORD result = kernel32::WaitForMultipleObjects(2, handles, FALSE, wait->milliseconds);
		if (result == WAIT_OBJECT_0 || result == WAIT_FAILED)
			break;
		if (result != WAIT_OBJECT_0 + 1 && result != WAIT_TIMEOUT)
			break;
		queueCallback(wait, result == WAIT_TIMEOUT ? TRUE : FALSE);
		if (wait->flags & kExecuteOnlyOnce)
			break;
	}
	wait->finishWait();
	return 0;
}

bool canRegisterOn(ObjectType type) {
	switch (type) {
	case ObjectType::Event:
	case ObjectType::MemoryResource:
	case ObjectType::Process:
	case ObjectType::ProcessThread:
	case ObjectType::Semaphore:
	case ObjectType::Thread:
	case ObjectType::Timer:
		return true;
	default:
		return false;
	}
}

} // namespace

namespace kernel32 {

BOOL WINAPI RegisterWaitForSingleObject(PHANDLE waitHandle, HANDLE object, WAITORTIMERCALLBACK callback, PVOID context,
										ULONG milliseconds, ULONG flags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegisterWaitForSingleObject(%p, %p, %p, %p, %u, 0x%x)\n", waitHandle, object, callback, context,
			  milliseconds, flags);
	if (!waitHandle || !callback) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (flags & ~kSupportedFlags) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	HandleMeta metadata{};
	auto target = wibo::handles().get(object, &metadata);
	if (!target || !canRegisterOn(target->type)) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!(metadata.grantedAccess & SYNCHRONIZE)) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}

	const DWORD previousError = getLastError();
	auto wait = std::make_shared<RegisteredWait>();
	wait->targetHandle = wibo::handles().alloc(std::move(target), metadata.grantedAccess, 0);
	wait->stop = make_pin<EventObject>(true);
	wait->stopHandle = wibo::handles().alloc(wait->stop.clone(), SYNCHRONIZE, 0);
	wait->callback = callback;
	wait->context = context;
	wait->milliseconds = milliseconds;
	wait->flags = flags;

	HANDLE registration;
	{
		std::lock_guard lock(g_waitsMutex);
		if (g_nextWaitHandle > 0x7FFFFFFB) {
			setLastError(ERROR_NOT_ENOUGH_MEMORY);
			return FALSE;
		}
		registration = static_cast<HANDLE>(g_nextWaitHandle);
		g_nextWaitHandle += 4;
		g_waits.emplace(registration, wait);
	}
	auto *parameter = new (std::nothrow) std::shared_ptr<RegisteredWait>(wait);
	DWORD error = ERROR_NOT_ENOUGH_MEMORY;
	if (!parameter || !createWorkerThread(&runWait, parameter, error)) {
		delete parameter;
		std::lock_guard lock(g_waitsMutex);
		g_waits.erase(registration);
		setLastError(error);
		return FALSE;
	}
	*waitHandle = registration;
	setLastError(previousError);
	return TRUE;
}

BOOL WINAPI UnregisterWaitEx(HANDLE waitHandle, HANDLE completionEvent) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("UnregisterWaitEx(%p, %p)\n", waitHandle, completionEvent);
	Pin<EventObject> completion;
	const bool blocking = completionEvent == INVALID_HANDLE_VALUE;
	if (completionEvent != NO_HANDLE && !blocking) {
		completion = wibo::handles().getAs<EventObject>(completionEvent);
		if (!completion) {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
	}
	const DWORD previousError = getLastError();
	std::shared_ptr<RegisteredWait> wait;
	{
		std::lock_guard lock(g_waitsMutex);
		auto it = g_waits.find(waitHandle);
		if (it == g_waits.end()) {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
		wait = std::move(it->second);
		g_waits.erase(it);
	}
	bool pending;
	bool done;
	{
		std::lock_guard lock(wait->mutex);
		wait->cancelled = true;
		wait->completion = std::move(completion);
		pending = wait->pendingCallbacks != 0;
		done = wait->waitDone;
	}
	wait->cv.notify_all();
	wait->stop->set();
	if (done && !pending && wait->completion)
		wait->completion->set();
	if (blocking) {
		if (g_activeCallback == wait.get()) {
			setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		std::unique_lock lock(wait->mutex);
		wait->cv.wait(lock, [&] { return wait->waitDone && !wait->pendingCallbacks; });
	}
	if (!blocking && pending && !wait->completion) {
		setLastError(ERROR_IO_PENDING);
		return FALSE;
	}
	setLastError(previousError);
	return TRUE;
}

BOOL WINAPI UnregisterWait(HANDLE waitHandle) { return UnregisterWaitEx(waitHandle, NO_HANDLE); }

} // namespace kernel32
