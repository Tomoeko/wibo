#include "processthreadsapi.h"

#include "common.h"
#include "completion_port.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "kernel32.h"
#include "kernel32_trampolines.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <pthread.h>
#include <thread>

namespace {
constexpr ULONG kLongFunction = 0x10;
constexpr ULONG kSupportedFlags = 0x01 | kLongFunction | 0x80;

class WorkQueue {
	struct Item {
		LPTHREAD_START_ROUTINE function;
		PVOID context;
		bool longFunction;
	};
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<Item> pending;
	size_t workers = 0, busy = 0, longFunctions = 0;
	size_t maximum = 512;
	const size_t concurrency = std::max(1U, std::thread::hardware_concurrency());

	bool addWorker(DWORD &error) {
		++workers;
		if (kernel32::createWorkerThread([](void *self) { return static_cast<WorkQueue *>(self)->run(); }, this, error))
			return true;
		--workers;
		return false;
	}

	DWORD run() {
		for (;;) {
			Item item{};
			{
				std::unique_lock lock(mutex);
				cv.wait(lock, [&] { return !pending.empty(); });
				item = pending.front();
				pending.pop_front();
				++busy;
			}
			call_LPTHREAD_START_ROUTINE(item.function, item.context);
			kernel32::detachCompletionThread();
			{
				std::lock_guard lock(mutex);
				--busy;
				if (item.longFunction)
					--longFunctions;
			}
			cv.notify_all();
		}
	}

	void watch() {
		std::unique_lock lock(mutex);
		for (;;) {
			cv.wait(lock, [&] { return !pending.empty(); });
			// Callbacks can block without a hint. Grow slowly when the ordinary workers are occupied.
			cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return pending.empty(); });
			if (!pending.empty() && pending.size() > workers - busy && workers < maximum) {
				DWORD error = ERROR_SUCCESS;
				addWorker(error);
			}
		}
	}

  public:
	bool initialize(DWORD &error) {
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
					static_cast<WorkQueue *>(self)->watch();
					return nullptr;
				},
				this);
		}
		pthread_attr_destroy(&attributes);
		error = wibo::winErrorFromErrno(result);
		return result == 0;
	}
	bool queue(LPTHREAD_START_ROUTINE function, PVOID context, ULONG flags, DWORD &error) {
		std::lock_guard lock(mutex);
		if (pending.size() == 1024 * 1024) {
			error = ERROR_NOT_ENOUGH_MEMORY;
			return false;
		}
		const auto requestedMaximum = flags >> 16;
		if (requestedMaximum)
			maximum = requestedMaximum;
		const bool longFunction = (flags & kLongFunction) != 0;
		pending.push_back({function, context, longFunction});
		if (longFunction)
			++longFunctions;
		const auto preferred = std::min(maximum, concurrency + longFunctions);
		if (pending.size() > workers - busy && workers < preferred && !addWorker(error) && !workers) {
			pending.pop_back();
			if (longFunction)
				--longFunctions;
			return false;
		}
		cv.notify_all();
		return true;
	}
};

WorkQueue *workQueue(DWORD &error) {
	// Callbacks may block indefinitely; the pool remains alive until process termination.
	static std::mutex initializationMutex;
	static WorkQueue *queue = nullptr;
	std::lock_guard lock(initializationMutex);
	if (!queue) {
		auto *created = new (std::nothrow) WorkQueue;
		if (!created) {
			error = ERROR_NOT_ENOUGH_MEMORY;
			return nullptr;
		}
		if (!created->initialize(error)) {
			delete created;
			return nullptr;
		}
		queue = created;
	}
	return queue;
}
} // namespace

namespace kernel32 {
BOOL WINAPI QueueUserWorkItem(LPTHREAD_START_ROUTINE function, PVOID context, ULONG flags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("QueueUserWorkItem(%p, %p, 0x%x)\n", function, context, flags);
	if (!function) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if ((flags & 0xFFFF & ~kSupportedFlags) != 0) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	const DWORD previousError = getLastError();
	DWORD error = ERROR_SUCCESS;
	auto *queue = workQueue(error);
	if (queue && queue->queue(function, context, flags, error)) {
		setLastError(previousError);
		return TRUE;
	}
	setLastError(error);
	return FALSE;
}
} // namespace kernel32
