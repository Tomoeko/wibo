#include "synchapi.h"

#include "common.h"
#include "completion_port.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "processthreadsapi.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace {
struct ConditionWaiter {
	std::condition_variable cv;
	bool notified = false;
};
std::mutex g_conditionMutex;
std::unordered_map<PCONDITION_VARIABLE, std::deque<ConditionWaiter *>> g_conditionWaiters;

void wake(PCONDITION_VARIABLE condition, bool all) {
	std::lock_guard lock(g_conditionMutex);
	auto it = g_conditionWaiters.find(condition);
	if (it == g_conditionWaiters.end())
		return;
	auto &waiters = it->second;
	do {
		auto *waiter = waiters.front();
		waiters.pop_front();
		waiter->notified = true;
		waiter->cv.notify_one();
	} while (all && !waiters.empty());
	if (waiters.empty())
		g_conditionWaiters.erase(it);
}
} // namespace

namespace kernel32 {
BOOL WINAPI SleepConditionVariableCS(PCONDITION_VARIABLE condition, PCRITICAL_SECTION section, DWORD milliseconds) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SleepConditionVariableCS(%p, %p, %u)\n", condition, section, milliseconds);
	if (!condition || !section || section->RecursionCount != 1 ||
		static_cast<ULONG_PTR>(section->OwningThread) != GetCurrentThreadId()) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ConditionWaiter waiter;
	bool notified;
	CompletionWait completionWait(milliseconds != 0);
	{
		std::unique_lock lock(g_conditionMutex);
		g_conditionWaiters[condition].push_back(&waiter);
		// Registration and lock release share the wake registry lock, so a wake
		// cannot pass between releasing the caller's lock and going to sleep.
		LeaveCriticalSection(section);
		const auto done = [&] { return waiter.notified; };
		if (milliseconds == INFINITE) {
			waiter.cv.wait(lock, done);
			notified = true;
		} else {
			notified = waiter.cv.wait_for(lock, std::chrono::milliseconds(milliseconds), done);
		}
		if (!notified) {
			auto it = g_conditionWaiters.find(condition);
			std::erase(it->second, &waiter);
			if (it->second.empty())
				g_conditionWaiters.erase(it);
		}
	}
	// Reacquisition may block behind the waking thread; do it without holding
	// the registry lock, which that thread may need for another wake.
	EnterCriticalSection(section);
	if (!notified)
		setLastError(ERROR_TIMEOUT);
	return notified ? TRUE : FALSE;
}

void WINAPI WakeConditionVariable(PCONDITION_VARIABLE condition) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WakeConditionVariable(%p)\n", condition);
	wake(condition, false);
}

void WINAPI WakeAllConditionVariable(PCONDITION_VARIABLE condition) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WakeAllConditionVariable(%p)\n", condition);
	wake(condition, true);
}
} // namespace kernel32
