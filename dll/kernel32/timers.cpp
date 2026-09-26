#include "synchapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "kernel32.h"
#include "strutil.h"
#include "timeutil.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {
using Clock = std::chrono::steady_clock;
using Ticks = std::chrono::duration<uint64_t, std::ratio<1, 10000000>>;
constexpr DWORD kTimerModify = 2;
constexpr DWORD kTimerAll = 0x1F0003;
class TimerScheduler;
std::atomic<TimerScheduler *> g_scheduler = nullptr;

uint64_t currentFileTime() {
	const auto ticks = std::chrono::duration_cast<std::chrono::duration<int64_t, Ticks::period>>(
						   std::chrono::system_clock::now().time_since_epoch())
						   .count();
	return static_cast<uint64_t>(ticks + static_cast<int64_t>(UNIX_TIME_ZERO));
}

class TimerScheduler {
	struct Entry {
		Pin<kernel32::TimerObject> object;
		Clock::time_point deadline;
		uint64_t absoluteTime;
		std::chrono::milliseconds period;
		bool absolute;
	};
	std::mutex mutex;
	std::condition_variable cv;
	std::unordered_map<kernel32::TimerObject *, Entry> entries;
	bool stopping = false;
	std::thread worker;

	static Clock::time_point deadlineFor(const Entry &entry, Clock::time_point now) {
		if (!entry.absolute)
			return entry.deadline;
		const auto current = currentFileTime();
		const auto ticks = entry.absoluteTime > current ? entry.absoluteTime - current : 0;
		const auto maximum = std::chrono::duration_cast<Ticks>(Clock::time_point::max() - now).count();
		return now + std::chrono::duration_cast<Clock::duration>(Ticks(std::min(ticks, maximum)));
	}

	void run() {
		std::unique_lock lock(mutex);
		while (!stopping) {
			if (entries.empty()) {
				cv.wait(lock, [&] { return stopping || !entries.empty(); });
				continue;
			}
			const auto now = Clock::now();
			auto earliest = Clock::time_point::max();
			for (const auto &[object, entry] : entries) {
				earliest = std::min(earliest, deadlineFor(entry, now));
				// Re-evaluate UTC deadlines when the wall clock changes.
				if (entry.absolute)
					earliest = std::min(earliest, now + std::chrono::seconds(1));
			}
			if (earliest > now) {
				cv.wait_until(lock, earliest);
				continue;
			}
			for (auto it = entries.begin(); it != entries.end();) {
				auto &entry = it->second;
				if (deadlineFor(entry, now) > now) {
					++it;
					continue;
				}
				auto object = entry.object.clone();
				{
					std::lock_guard objectLock(object->m);
					object->signaled = true;
				}
				if (object->manualReset)
					object->cv.notify_all();
				else
					object->cv.notify_one();
				object->notifyWaiters(false);
				if (!entry.period.count())
					it = entries.erase(it);
				else {
					entry.absolute = false;
					entry.deadline = now + entry.period;
					++it;
				}
			}
		}
	}

  public:
	TimerScheduler() : worker([this] { run(); }) { g_scheduler.store(this); }
	~TimerScheduler() {
		g_scheduler.store(nullptr);
		{
			std::lock_guard lock(mutex);
			stopping = true;
		}
		cv.notify_one();
		worker.join();
	}
	bool arm(Pin<kernel32::TimerObject> object, int64_t due, LONG period) {
		std::lock_guard lock(mutex);
		if (!object->handleCount.load()) {
			kernel32::setLastError(ERROR_INVALID_HANDLE);
			return false;
		}
		const auto now = Clock::now();
		const auto current = currentFileTime();
		const auto ticks = due < 0								  ? uint64_t{0} - static_cast<uint64_t>(due)
						   : static_cast<uint64_t>(due) > current ? static_cast<uint64_t>(due) - current
																  : 0;
		if (ticks > std::chrono::duration_cast<Ticks>(Clock::time_point::max() - now).count()) {
			kernel32::setLastError(ERROR_NOT_SUPPORTED);
			return false;
		}
		const auto deadline = now + std::chrono::duration_cast<Clock::duration>(Ticks(ticks));
		{
			std::lock_guard objectLock(object->m);
			object->signaled = false;
		}
		auto *key = object.get();
		entries.insert_or_assign(key, Entry{std::move(object), deadline, static_cast<uint64_t>(due),
											std::chrono::milliseconds(period), due >= 0});
		cv.notify_one();
		return true;
	}
	void cancel(kernel32::TimerObject *object) {
		std::lock_guard lock(mutex);
		entries.erase(object);
		cv.notify_one();
	}
};

TimerScheduler &timerScheduler() {
	static TimerScheduler scheduler;
	return scheduler;
}

Pin<kernel32::TimerObject> getTimer(HANDLE handle) {
	HandleMeta metadata{};
	auto object = wibo::handles().get(handle, &metadata).downcast<kernel32::TimerObject>();
	if (!object)
		kernel32::setLastError(ERROR_INVALID_HANDLE);
	else if (!(metadata.grantedAccess & kTimerModify)) {
		kernel32::setLastError(ERROR_ACCESS_DENIED);
		object.reset();
	}
	return object;
}
} // namespace

namespace kernel32 {
void TimerObject::onLastHandleClosed() noexcept {
	if (auto *scheduler = g_scheduler.load())
		scheduler->cancel(this);
}

HANDLE WINAPI CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags, DWORD access) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateWaitableTimerExW(%p, %p, %u, 0x%x)\n", attributes, name, flags, access);
	if ((flags & ~3U) || (attributes && attributes->nLength != sizeof(SECURITY_ATTRIBUTES))) {
		setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}
	if (attributes && attributes->lpSecurityDescriptor) {
		setLastError(ERROR_NOT_SUPPORTED);
		return NO_HANDLE;
	}
	std::u16string objectName;
	if (name) {
		for (size_t index = 0; name[index]; ++index) {
			if (index == 260) {
				setLastError(206);
				return NO_HANDLE;
			}
			objectName.push_back(name[index]);
		}
		auto suffix = objectName;
		if (suffix.starts_with(u"Global\\") || suffix.starts_with(u"Local\\"))
			suffix.erase(0, suffix.find(u'\\') + 1);
		if (suffix.find(u'\\') != std::u16string::npos) {
			setLastError(ERROR_INVALID_NAME);
			return NO_HANDLE;
		}
	}
	if (access & 0x80000000U)
		access = (access & ~0x80000000U) | 0x20001;
	if (access & 0x40000000U)
		access = (access & ~0x40000000U) | 0x20002;
	if (access & 0x20000000U)
		access = (access & ~0x20000000U) | 0x120000;
	if (access & 0x10000000U)
		access = (access & ~0x10000000U) | kTimerAll;
	if (access & ~kTimerAll) {
		setLastError(ERROR_NOT_SUPPORTED);
		return NO_HANDLE;
	}
	auto [object, created] = wibo::g_namespace.getOrCreate(objectName, [&] { return new TimerObject(flags & 1); });
	if (!object) {
		setLastError(ERROR_INVALID_HANDLE);
		return NO_HANDLE;
	}
	const auto handle = wibo::handles().alloc(std::move(object), access,
											  attributes && attributes->bInheritHandle ? HANDLE_FLAG_INHERIT : 0);
	setLastError(created ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS);
	return handle;
}

BOOL WINAPI SetWaitableTimer(HANDLE handle, const LARGE_INTEGER *dueTime, LONG period, GUEST_PTR callback,
							 LPVOID callbackArgument, BOOL resume) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetWaitableTimer(%p, %p, %d, 0x%llx, %p, %u)\n", handle, dueTime, period,
			  static_cast<unsigned long long>(callback), callbackArgument, resume);
	auto object = getTimer(handle);
	if (!object)
		return FALSE;
	if (!dueTime || period < 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (callback) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	int64_t due = 0;
	std::memcpy(&due, dueTime, sizeof(due));
	if (!timerScheduler().arm(std::move(object), due, period))
		return FALSE;
	if (resume)
		setLastError(ERROR_NOT_SUPPORTED);
	return TRUE;
}

BOOL WINAPI CancelWaitableTimer(HANDLE handle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CancelWaitableTimer(%p)\n", handle);
	auto object = getTimer(handle);
	if (!object)
		return FALSE;
	if (auto *scheduler = g_scheduler.load())
		scheduler->cancel(object.get());
	return TRUE;
}
} // namespace kernel32
