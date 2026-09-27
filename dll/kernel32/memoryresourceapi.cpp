#include "memoryresourceapi.h"

#include "context.h"
#include "errors.h"
#include "internal.h"
#include "system_provider.h"

#include <array>
#include <chrono>
#include <thread>

namespace kernel32 {
struct MemoryResourceMonitorSession {
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<MemoryResourceObject *> objects;
	bool stopping = false;
	std::thread worker;
};
} // namespace kernel32

namespace {
using kernel32::MemoryResourceMonitorSession;
using kernel32::MemoryResourceObject;
using MemoryStates = std::array<uint32_t, 2>;
std::atomic<uint64_t> g_sampleGeneration{0};
std::timed_mutex g_sampleMutex;
class MemoryResourceMonitor;
std::atomic<MemoryResourceMonitor *> g_monitor = nullptr;

DWORD sampleMemoryState(MemoryStates &states, uint64_t &generation, int timeoutMs = 500) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	std::unique_lock lock(g_sampleMutex, std::defer_lock);
	if (!lock.try_lock_until(deadline))
		return ERROR_TIMEOUT;
	generation = g_sampleGeneration.fetch_add(1) + 1;
	std::vector<uint8_t> response;
	const auto remaining =
		std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
	bool timedOut = false;
	if (remaining.count() <= 0)
		return ERROR_TIMEOUT;
	if (!wibo::provider::request({"memory-resource-state"}, response, static_cast<int>(remaining.count()), &timedOut))
		return timedOut ? ERROR_TIMEOUT : ERROR_NOT_SUPPORTED;
	wibo::provider::Reader reader(response);
	int32_t error = 0;
	if (!reader.header(error))
		return ERROR_INVALID_DATA;
	if (error)
		return reader.done() ? static_cast<DWORD>(error) : ERROR_INVALID_DATA;
	if (!reader.number(states[0]) || !reader.number(states[1]) || !reader.done() || states[0] > 1 || states[1] > 1)
		return ERROR_INVALID_DATA;
	return 0;
}

class MemoryResourceMonitor {
	std::mutex mutex;
	std::vector<std::shared_ptr<MemoryResourceMonitorSession>> sessions;
	std::shared_ptr<MemoryResourceMonitorSession> current;

	static void run(const std::shared_ptr<MemoryResourceMonitorSession> &session) {
		std::unique_lock lock(session->mutex);
		while (!session->stopping) {
			if (session->cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return session->stopping; }))
				break;
			std::vector<Pin<MemoryResourceObject>> objects;
			for (auto *object : session->objects)
				objects.push_back(Pin<MemoryResourceObject>::acquire(object));
			lock.unlock();
			MemoryStates states{};
			uint64_t generation = 0;
			const DWORD error = sampleMemoryState(states, generation);
			for (auto &object : objects)
				object->publish(states[object->notificationType] != 0, error, generation);
			lock.lock();
			if (error)
				session->stopping = true;
		}
	}

  public:
	MemoryResourceMonitor() { g_monitor.store(this); }
	~MemoryResourceMonitor() {
		g_monitor.store(nullptr);
		std::vector<std::thread> workers;
		{
			std::lock_guard lock(mutex);
			for (const auto &session : sessions) {
				std::lock_guard sessionLock(session->mutex);
				session->stopping = true;
				session->cv.notify_all();
				workers.push_back(std::move(session->worker));
			}
			sessions.clear();
			current.reset();
		}
		for (auto &worker : workers)
			worker.join();
	}

	void add(MemoryResourceObject &object) {
		std::lock_guard lock(mutex);
		if (current) {
			std::lock_guard sessionLock(current->mutex);
			if (current->stopping)
				current.reset();
		}
		const bool start = !current;
		if (start) {
			current = std::make_shared<MemoryResourceMonitorSession>();
			sessions.push_back(current);
		}
		object.session = current;
		{
			std::lock_guard sessionLock(current->mutex);
			current->objects.push_back(&object);
		}
		if (start)
			current->worker = std::thread([session = current] { run(session); });
	}

	void remove(MemoryResourceObject &object) {
		std::thread worker;
		{
			std::lock_guard lock(mutex);
			auto session = object.session;
			if (!session)
				return;
			std::lock_guard sessionLock(session->mutex);
			std::erase(session->objects, &object);
			if (!session->objects.empty())
				return;
			session->stopping = true;
			session->cv.notify_all();
			worker = std::move(session->worker);
			std::erase(sessions, session);
			if (current == session)
				current.reset();
		}
		if (worker.joinable())
			worker.join();
	}

	static void fail(const std::shared_ptr<MemoryResourceMonitorSession> &session, DWORD error, uint64_t generation) {
		std::vector<Pin<MemoryResourceObject>> objects;
		{
			std::lock_guard lock(session->mutex);
			session->stopping = true;
			session->cv.notify_all();
			for (auto *object : session->objects)
				objects.push_back(Pin<MemoryResourceObject>::acquire(object));
		}
		for (auto &object : objects)
			object->publish(false, error, generation);
	}
};

MemoryResourceMonitor &monitor() {
	static MemoryResourceMonitor value;
	return value;
}
} // namespace

namespace kernel32 {

void MemoryResourceObject::publish(bool state, DWORD error, uint64_t generation) {
	bool changed = false;
	{
		std::lock_guard lock(m);
		if (backendError || (!error && generation <= sampleGeneration))
			return;
		sampleGeneration = generation;
		changed = backendError != error || signaled != state;
		backendError = error;
		if (!error)
			signaled = state;
	}
	if (changed) {
		cv.notify_all();
		notifyWaiters(false);
	}
}

void MemoryResourceObject::onLastHandleClosed() noexcept {
	if (auto *value = g_monitor.load())
		value->remove(*this);
}

HANDLE WINAPI CreateMemoryResourceNotification(DWORD notificationType) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateMemoryResourceNotification(%u)\n", notificationType);
	if (notificationType > 1) {
		setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}
	MemoryStates states{};
	uint64_t generation = 0;
	const DWORD error = sampleMemoryState(states, generation, 2000);
	if (error) {
		setLastError(error);
		return NO_HANDLE;
	}
	auto object = make_pin<MemoryResourceObject>(notificationType);
	object->signaled = states[notificationType] != 0;
	object->sampleGeneration = generation;
	const HANDLE result = wibo::handles().alloc(object.clone(), 0x1f0001, 0);
	monitor().add(*object);
	return result;
}

BOOL WINAPI QueryMemoryResourceNotification(HANDLE notification, BOOL *state) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("QueryMemoryResourceNotification(%p, %p)\n", notification, state);
	if (!state) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	auto object = wibo::handles().get(notification);
	if (auto event = object.clone().downcast<EventObject>()) {
		std::lock_guard lock(event->m);
		*state = event->signaled;
		return TRUE;
	}
	auto memory = std::move(object).downcast<MemoryResourceObject>();
	if (!memory) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	{
		std::lock_guard lock(memory->m);
		if (memory->backendError) {
			setLastError(memory->backendError);
			return FALSE;
		}
	}
	MemoryStates states{};
	uint64_t generation = 0;
	const DWORD error = sampleMemoryState(states, generation);
	if (error)
		MemoryResourceMonitor::fail(memory->session, error, generation);
	else
		memory->publish(states[memory->notificationType] != 0, 0, generation);
	std::lock_guard lock(memory->m);
	if (memory->backendError) {
		setLastError(memory->backendError);
		return FALSE;
	}
	*state = memory->signaled;
	return TRUE;
}

} // namespace kernel32
