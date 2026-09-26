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
template <class Release, class Acquire>
BOOL sleepOnCondition(PCONDITION_VARIABLE condition, DWORD milliseconds, Release release, Acquire acquire) {
	ConditionWaiter waiter;
	bool notified;
	kernel32::CompletionWait completionWait(milliseconds != 0);
	{
		std::unique_lock lock(g_conditionMutex);
		g_conditionWaiters[condition].push_back(&waiter);
		// Registration and lock release share the wake registry lock, so a wake
		// cannot pass between releasing the caller's lock and going to sleep.
		release();
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
	acquire();
	if (!notified)
		kernel32::setLastError(ERROR_TIMEOUT);
	return notified ? TRUE : FALSE;
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
	return sleepOnCondition(
		condition, milliseconds, [&] { LeaveCriticalSection(section); }, [&] { EnterCriticalSection(section); });
}

BOOL WINAPI SleepConditionVariableSRW(PCONDITION_VARIABLE condition, PSRWLOCK lock, DWORD milliseconds, ULONG flags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SleepConditionVariableSRW(%p, %p, %u, 0x%x)\n", condition, lock, milliseconds, flags);
	constexpr ULONG shared = 0x1; // CONDITION_VARIABLE_LOCKMODE_SHARED
	if (!condition || !lock || (flags & ~shared)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return sleepOnCondition(
		condition, milliseconds,
		[&] {
			if (flags & shared)
				ReleaseSRWLockShared(lock);
			else
				ReleaseSRWLockExclusive(lock);
		},
		[&] {
			if (flags & shared)
				AcquireSRWLockShared(lock);
			else
				AcquireSRWLockExclusive(lock);
		});
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
