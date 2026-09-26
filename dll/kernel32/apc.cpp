#include "internal.h"

#include "context.h"
#include "errors.h"
#include "kernel32.h"

#include <chrono>
#include <thread>

namespace {
struct CurrentQueue {
	std::shared_ptr<kernel32::ApcState> state;
	~CurrentQueue() { kernel32::closeApcState(); }
};
thread_local CurrentQueue g_currentQueue;

struct WakeRegistration {
	std::mutex mutex;
	std::condition_variable cv;
	uint64_t generation = 0;
	WaitableObject *object;
	kernel32::ApcState *queue;

	static void notify(void *context, WaitableObject *, DWORD, bool) {
		auto &wake = *static_cast<WakeRegistration *>(context);
		std::lock_guard lock(wake.mutex);
		++wake.generation;
		wake.cv.notify_one();
	}
	WakeRegistration(WaitableObject *object, kernel32::ApcState *queue) : object(object), queue(queue) {
		queue->registerWaiter(this, 0, &notify);
		if (object)
			object->registerWaiter(this, 0, &notify);
	}
	~WakeRegistration() {
		if (object)
			object->unregisterWaiter(this);
		queue->unregisterWaiter(this);
	}
};
} // namespace

namespace kernel32 {
std::shared_ptr<ApcState> currentApcState() {
	if (!g_currentQueue.state)
		g_currentQueue.state = std::make_shared<ApcState>();
	return g_currentQueue.state;
}

void installApcState(std::shared_ptr<ApcState> state) { g_currentQueue.state = std::move(state); }

void closeApcState() {
	if (auto &state = g_currentQueue.state) {
		std::lock_guard lock(state->m);
		state->terminated = true;
		state->pending.clear();
	}
}

bool dispatchPendingApcs() {
	auto state = currentApcState();
	bool dispatched = false;
	for (;;) {
		ApcState::Entry entry{};
		{
			std::lock_guard lock(state->m);
			if (state->pending.empty())
				return dispatched;
			entry = state->pending.front();
			state->pending.pop_front();
		}
		dispatched = true;
		call_PAPCFUNC(reinterpret_cast<PAPCFUNC>(entry.callback), entry.argument);
	}
}

DWORD WINAPI QueueUserAPC(PAPCFUNC callback, HANDLE thread, ULONG_PTR argument) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("QueueUserAPC(%p, %p, 0x%llx)\n", callback, thread, static_cast<unsigned long long>(argument));
	if (!callback) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::shared_ptr<ApcState> state;
	if (isPseudoCurrentThreadHandle(thread))
		state = currentApcState();
	else {
		HandleMeta metadata{};
		auto object = wibo::handles().getAs<ThreadObject>(thread, &metadata);
		if (!object) {
			setLastError(ERROR_INVALID_HANDLE);
			return 0;
		}
		if (!(metadata.grantedAccess & 0x10)) {
			setLastError(ERROR_ACCESS_DENIED);
			return 0;
		}
		state = object->apc;
	}
	{
		std::lock_guard lock(state->m);
		if (state->terminated) {
			setLastError(ERROR_GEN_FAILURE);
			return 0;
		}
		state->pending.push_back({toGuestPtr(reinterpret_cast<void *>(callback)), argument});
	}
	state->notifyWaiters(false);
	return 1;
}

DWORD waitAlertable(HANDLE handle, WaitableObject *object, DWORD milliseconds) {
	auto queue = currentApcState();
	WakeRegistration wake(object, queue.get());
	const auto deadline = milliseconds == INFINITE
							  ? std::chrono::steady_clock::time_point::max()
							  : std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	for (;;) {
		uint64_t generation;
		{
			std::lock_guard lock(wake.mutex);
			generation = wake.generation;
		}
		if (object) {
			const auto result = WaitForSingleObject(handle, 0);
			if (result != WAIT_TIMEOUT)
				return result;
		}
		if (dispatchPendingApcs())
			return WAIT_IO_COMPLETION;
		if (!milliseconds || (milliseconds != INFINITE && std::chrono::steady_clock::now() >= deadline))
			return WAIT_TIMEOUT;
		std::unique_lock lock(wake.mutex);
		auto changed = [&] { return generation != wake.generation; };
		if (milliseconds == INFINITE)
			wake.cv.wait(lock, changed);
		else if (!wake.cv.wait_until(lock, deadline, changed))
			return WAIT_TIMEOUT;
	}
}

DWORD WINAPI SleepEx(DWORD milliseconds, BOOL alertable) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SleepEx(%u, %u)\n", milliseconds, alertable);
	if (!alertable) {
		Sleep(milliseconds);
		return 0;
	}
	const auto result = waitAlertable(NO_HANDLE, nullptr, milliseconds);
	if (!milliseconds && result == WAIT_TIMEOUT)
		std::this_thread::yield();
	return result == WAIT_TIMEOUT ? 0 : result;
}
} // namespace kernel32
