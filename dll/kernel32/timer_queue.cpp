#include "synchapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "kernel32.h"
#include "kernel32_trampolines.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <pthread.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr ULONG kLongFunction = 0x10;
constexpr ULONG kExecuteOnlyOnce = 0x08;
constexpr ULONG kSupportedFlags = kLongFunction | kExecuteOnlyOnce;
constexpr size_t kMaxConcurrentCallbacks = 64;
constexpr uint32_t kFirstQueue = 0x60000000;
constexpr uint32_t kLastQueue = 0x6fffffff;
constexpr uint32_t kFirstTimer = 0x70000000;
constexpr uint32_t kLastTimer = 0x7fffffff;

struct Timer {
	HANDLE queue;
	WAITORTIMERCALLBACK callback;
	PVOID parameter;
	Clock::time_point deadline;
	std::chrono::milliseconds period;
	bool armed = true;
	bool deleting = false;
	size_t callbacks = 0;
	Pin<kernel32::EventObject> completion;
	std::condition_variable finished;
};

class TimerQueues {
	std::mutex mutex;
	std::condition_variable changed;
	std::unordered_set<HANDLE> queues;
	std::unordered_map<HANDLE, std::shared_ptr<Timer>> timers;
	size_t activeCallbacks = 0;
	uint32_t nextQueue = kFirstQueue;
	uint32_t nextTimer = kFirstTimer;

	struct CallbackTask {
		TimerQueues *owner;
		std::shared_ptr<Timer> timer;
	};

	static DWORD callbackWorker(void *parameter) {
		std::unique_ptr<CallbackTask> task(static_cast<CallbackTask *>(parameter));
		call_WAITORTIMERCALLBACK(task->timer->callback, task->timer->parameter, TRUE);
		task->owner->callbackFinished(task->timer);
		return 0;
	}

	void callbackFinished(const std::shared_ptr<Timer> &timer) {
		Pin<kernel32::EventObject> completion;
		{
			std::lock_guard lock(mutex);
			--timer->callbacks;
			--activeCallbacks;
			changed.notify_one();
			if (timer->deleting && timer->callbacks == 0) {
				completion = std::move(timer->completion);
				timer->finished.notify_all();
			}
		}
		if (completion)
			completion->set();
	}

	void launch(const std::shared_ptr<Timer> &timer) {
		auto *task = new (std::nothrow) CallbackTask{this, timer};
		DWORD error = ERROR_NOT_ENOUGH_MEMORY;
		if (task && kernel32::createWorkerThread(callbackWorker, task, error))
			return;
		delete task;
		Pin<kernel32::EventObject> completion;
		{
			std::lock_guard lock(mutex);
			--timer->callbacks;
			--activeCallbacks;
			changed.notify_one();
			if (!timer->deleting && !timer->armed) {
				// Retry a one-shot expiration if a worker could not be created.
				timer->armed = true;
				timer->deadline = Clock::now() + std::chrono::milliseconds(100);
				changed.notify_one();
			} else if (timer->deleting && timer->callbacks == 0) {
				completion = std::move(timer->completion);
				timer->finished.notify_all();
			}
		}
		if (completion)
			completion->set();
	}

	void run() {
		std::unique_lock lock(mutex);
		for (;;) {
			auto earliest = Clock::time_point::max();
			for (const auto &[handle, timer] : timers)
				if (timer->armed && timer->deadline < earliest)
					earliest = timer->deadline;
			if (earliest == Clock::time_point::max()) {
				changed.wait(lock);
				continue;
			}
			const auto now = Clock::now();
			if (earliest > now) {
				changed.wait_until(lock, earliest);
				continue;
			}
			if (activeCallbacks == kMaxConcurrentCallbacks) {
				changed.wait(lock, [&] { return activeCallbacks < kMaxConcurrentCallbacks; });
				continue;
			}
			std::vector<std::shared_ptr<Timer>> ready;
			for (const auto &[handle, timer] : timers) {
				if (!timer->armed || timer->deadline > now)
					continue;
				if (activeCallbacks == kMaxConcurrentCallbacks)
					break;
				++timer->callbacks;
				++activeCallbacks;
				ready.push_back(timer);
				if (timer->period.count())
					timer->deadline = now + timer->period;
				else
					timer->armed = false;
			}
			lock.unlock();
			for (const auto &timer : ready)
				launch(timer);
			lock.lock();
		}
	}

  public:
	bool start(DWORD &error) {
		pthread_attr_t attributes;
		int result = pthread_attr_init(&attributes);
		if (result) {
			error = wibo::winErrorFromErrno(result);
			return false;
		}
		result = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
		if (!result) {
			pthread_t thread;
			result = pthread_create(
				&thread, &attributes,
				[](void *self) -> void * {
					static_cast<TimerQueues *>(self)->run();
					return nullptr;
				},
				this);
		}
		pthread_attr_destroy(&attributes);
		error = wibo::winErrorFromErrno(result);
		return result == 0;
	}

	HANDLE createQueue() {
		std::lock_guard lock(mutex);
		if (nextQueue > kLastQueue)
			return NO_HANDLE;
		const HANDLE queue = nextQueue++;
		queues.insert(queue);
		return queue;
	}

	bool createTimer(PHANDLE output, HANDLE queue, WAITORTIMERCALLBACK callback, PVOID parameter, DWORD dueTime,
					 DWORD period, DWORD &error) {
		std::lock_guard lock(mutex);
		if (queue && !queues.contains(queue)) {
			error = ERROR_INVALID_HANDLE;
			return false;
		}
		if (nextTimer > kLastTimer) {
			error = ERROR_NOT_ENOUGH_MEMORY;
			return false;
		}
		const HANDLE handle = nextTimer++;
		auto timer = std::make_shared<Timer>();
		timer->queue = queue;
		timer->callback = callback;
		timer->parameter = parameter;
		timer->deadline = Clock::now() + std::chrono::milliseconds(dueTime);
		timer->period = std::chrono::milliseconds(period);
		timers.emplace(handle, std::move(timer));
		*output = handle;
		changed.notify_one();
		return true;
	}

	bool changeTimer(HANDLE queue, HANDLE handle, ULONG dueTime, ULONG period) {
		std::lock_guard lock(mutex);
		auto it = timers.find(handle);
		if (it == timers.end() || it->second->queue != queue)
			return false;
		auto &timer = *it->second;
		if (!timer.armed)
			return true; // Windows does not rearm an expired one-shot timer.
		timer.deadline = Clock::now() + std::chrono::milliseconds(dueTime);
		timer.period = std::chrono::milliseconds(period);
		changed.notify_one();
		return true;
	}

	bool deleteTimer(HANDLE queue, HANDLE handle, HANDLE completionHandle, DWORD &error) {
		Pin<kernel32::EventObject> event;
		if (completionHandle && completionHandle != INVALID_HANDLE_VALUE) {
			event = wibo::handles().getAs<kernel32::EventObject>(completionHandle);
			if (!event) {
				error = ERROR_INVALID_HANDLE;
				return false;
			}
		}
		std::shared_ptr<Timer> timer;
		{
			std::unique_lock lock(mutex);
			auto it = timers.find(handle);
			if (it == timers.end() || it->second->queue != queue) {
				error = ERROR_INVALID_HANDLE;
				return false;
			}
			timer = it->second;
			timer->deleting = true;
			timer->armed = false;
			timers.erase(it);
			changed.notify_one();
			if (timer->callbacks && completionHandle == INVALID_HANDLE_VALUE)
				timer->finished.wait(lock, [&] { return timer->callbacks == 0; });
			else if (timer->callbacks && completionHandle) {
				timer->completion = std::move(event);
				return true;
			} else if (timer->callbacks) {
				error = ERROR_IO_PENDING;
				return false;
			}
		}
		if (event)
			event->set();
		return true;
	}
};

TimerQueues *timerQueues(DWORD &error) {
	static std::mutex initializationMutex;
	static TimerQueues *state = nullptr;
	std::lock_guard lock(initializationMutex);
	if (!state) {
		auto *created = new (std::nothrow) TimerQueues;
		if (!created) {
			error = ERROR_NOT_ENOUGH_MEMORY;
			return nullptr;
		}
		if (!created->start(error)) {
			delete created;
			return nullptr;
		}
		state = created;
	}
	return state;
}

} // namespace

namespace kernel32 {

HANDLE WINAPI CreateTimerQueue() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateTimerQueue()\n");
	const DWORD previousError = getLastError();
	DWORD error = ERROR_SUCCESS;
	auto *state = timerQueues(error);
	if (!state) {
		setLastError(error);
		return NO_HANDLE;
	}
	const HANDLE queue = state->createQueue();
	if (!queue) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NO_HANDLE;
	}
	setLastError(previousError);
	return queue;
}

BOOL WINAPI CreateTimerQueueTimer(PHANDLE timer, HANDLE queue, WAITORTIMERCALLBACK callback, PVOID parameter,
								  DWORD dueTime, DWORD period, ULONG flags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateTimerQueueTimer(%p, %p, %p, %p, %u, %u, 0x%x)\n", timer, queue, callback, parameter, dueTime,
			  period, flags);
	if (!timer || !callback || ((flags & kExecuteOnlyOnce) && period)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (flags & ~kSupportedFlags) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	const DWORD previousError = getLastError();
	DWORD error = ERROR_SUCCESS;
	auto *state = timerQueues(error);
	if (!state) {
		setLastError(error);
		return FALSE;
	}
	if (!state->createTimer(timer, queue, callback, parameter, dueTime, period, error)) {
		setLastError(error);
		return FALSE;
	}
	setLastError(previousError);
	return TRUE;
}

BOOL WINAPI ChangeTimerQueueTimer(HANDLE queue, HANDLE timer, ULONG dueTime, ULONG period) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ChangeTimerQueueTimer(%p, %p, %u, %u)\n", queue, timer, dueTime, period);
	const DWORD previousError = getLastError();
	DWORD error = ERROR_SUCCESS;
	auto *state = timerQueues(error);
	if (!state) {
		setLastError(error);
		return FALSE;
	}
	if (!state->changeTimer(queue, timer, dueTime, period)) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	setLastError(previousError);
	return TRUE;
}

BOOL WINAPI DeleteTimerQueueTimer(HANDLE queue, HANDLE timer, HANDLE completionEvent) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DeleteTimerQueueTimer(%p, %p, %p)\n", queue, timer, completionEvent);
	const DWORD previousError = getLastError();
	DWORD error = ERROR_SUCCESS;
	auto *state = timerQueues(error);
	if (!state) {
		setLastError(error);
		return FALSE;
	}
	if (!state->deleteTimer(queue, timer, completionEvent, error)) {
		setLastError(error);
		return FALSE;
	}
	setLastError(previousError);
	return TRUE;
}

} // namespace kernel32
