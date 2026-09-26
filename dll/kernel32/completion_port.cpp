#include "completion_port.h"

#include "context.h"
#include "errors.h"
#include "internal.h"
#include "ioapiset.h"
#include "ntdll.h"
#include "synchapi.h"
#include "ws2/internal.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace {
constexpr DWORD kPortAccess = 0x1F0003;
struct ThreadAssociation {
	Pin<kernel32::CompletionPortObject> port;
	bool active = false;
	~ThreadAssociation() { kernel32::detachCompletionThread(); }
};
thread_local ThreadAssociation g_association;
} // namespace

namespace kernel32 {
void CompletionPortObject::dispatch() {
	while (!closed && !packets.empty() && !waiters.empty() && active < concurrency) {
		auto *waiter = waiters.back();
		waiters.pop_back();
		waiter->packet = packets.front();
		packets.pop_front();
		waiter->ready = true;
		++active;
		waiter->cv.notify_one();
	}
}

bool CompletionPortObject::post(CompletionPacket packet) {
	std::lock_guard lock(mutex);
	if (closed)
		return false;
	packets.push_back(packet);
	dispatch();
	return true;
}

void CompletionPortObject::onLastHandleClosed() noexcept {
	std::lock_guard lock(mutex);
	closed = true;
	for (auto *waiter : waiters)
		waiter->cv.notify_one();
}

void detachCompletionThread() {
	auto port = std::move(g_association.port);
	if (port && std::exchange(g_association.active, false)) {
		std::lock_guard lock(port->mutex);
		--port->active;
		port->dispatch();
	}
}

CompletionWait::CompletionWait(bool blocking) {
	if (blocking && g_association.active && g_association.port) {
		port = g_association.port.clone();
		std::lock_guard lock(port->mutex);
		g_association.active = false;
		--port->active;
		port->dispatch();
	}
}

CompletionWait::~CompletionWait() {
	if (port && port.get() == g_association.port.get() && !g_association.active) {
		std::lock_guard lock(port->mutex);
		++port->active;
		g_association.active = true;
	}
}

HANDLE WINAPI CreateIoCompletionPort(HANDLE fileHandle, HANDLE existingPort, ULONG_PTR key, DWORD concurrency) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateIoCompletionPort(%p, %p, 0x%llx, %u)\n", fileHandle, existingPort,
			  static_cast<unsigned long long>(key), concurrency);
	if (fileHandle == INVALID_HANDLE_VALUE && existingPort) {
		setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}
	Pin<FsObject> file;
	std::shared_ptr<ws2::detail::Socket> socket;
	std::shared_ptr<const CompletionBinding> *completion = nullptr;
	if (fileHandle != INVALID_HANDLE_VALUE) {
		file = wibo::handles().getAs<FsObject>(fileHandle);
		bool overlapped = false;
		if (file && file->valid()) {
			completion = &file->completion;
			overlapped = file->overlapped;
		} else {
			socket = ws2::detail::findSocket(static_cast<SOCKET>(fileHandle));
			if (!socket) {
				setLastError(ERROR_INVALID_HANDLE);
				return NO_HANDLE;
			}
			completion = &socket->completion;
			overlapped = socket->overlapped;
		}
		if (!overlapped) {
			setLastError(ERROR_INVALID_PARAMETER);
			return NO_HANDLE;
		}
	}
	Pin<CompletionPortObject> port;
	if (existingPort) {
		port = wibo::handles().getAs<CompletionPortObject>(existingPort);
		if (!port) {
			setLastError(ERROR_INVALID_HANDLE);
			return NO_HANDLE;
		}
	} else {
		if (!concurrency)
			concurrency = std::max(1U, std::thread::hardware_concurrency());
		port = make_pin<CompletionPortObject>(concurrency);
	}
	if (completion) {
		std::shared_ptr<const CompletionBinding> expected;
		auto binding = std::make_shared<const CompletionBinding>(port.clone(), key);
		if (!std::atomic_compare_exchange_strong(completion, &expected, std::move(binding))) {
			setLastError(ERROR_INVALID_PARAMETER);
			return NO_HANDLE;
		}
	}
	return existingPort ? existingPort : wibo::handles().alloc(std::move(port), kPortAccess, 0);
}

BOOL WINAPI PostQueuedCompletionStatus(HANDLE handle, DWORD bytes, ULONG_PTR key, LPOVERLAPPED overlapped) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("PostQueuedCompletionStatus(%p, %u, 0x%llx, %p)\n", handle, bytes, static_cast<unsigned long long>(key),
			  overlapped);
	HandleMeta meta{};
	auto port = wibo::handles().getAs<CompletionPortObject>(handle, &meta);
	if (!port) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!(meta.grantedAccess & 2)) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	if (!port->post({bytes, key, toGuestPtr(overlapped), STATUS_SUCCESS})) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI GetQueuedCompletionStatus(HANDLE handle, LPDWORD bytes, ULONG_PTR *key, guest_ptr<OVERLAPPED> *overlapped,
									  DWORD milliseconds) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetQueuedCompletionStatus(%p, %p, %p, %p, %u)\n", handle, bytes, key, overlapped, milliseconds);
	if (!bytes || !key || !overlapped) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	overlapped->ptr = GUEST_NULL;
	HandleMeta meta{};
	auto port = wibo::handles().getAs<CompletionPortObject>(handle, &meta);
	if (!port) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!(meta.grantedAccess & 2)) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	detachCompletionThread();
	g_association.port = port.clone();
	std::unique_lock lock(port->mutex);
	CompletionPortObject::Waiter waiter;
	if (!port->closed) {
		port->waiters.push_back(&waiter);
		port->dispatch();
		auto done = [&] { return waiter.ready || port->closed; };
		if (milliseconds == INFINITE)
			waiter.cv.wait(lock, done);
		else
			waiter.cv.wait_for(lock, std::chrono::milliseconds(milliseconds), done);
	}
	if (!waiter.ready) {
		std::erase(port->waiters, &waiter);
		if (!port->closed) {
			++port->active;
			g_association.active = true;
		}
		setLastError(port->closed ? ERROR_ABANDONED_WAIT_0 : WAIT_TIMEOUT);
		return FALSE;
	}
	g_association.active = true;
	const auto packet = waiter.packet;
	lock.unlock();
	*bytes = packet.bytes;
	*key = packet.key;
	overlapped->ptr = packet.context;
	if (packet.status >= 0)
		return TRUE;
	DWORD error = wibo::winErrorFromNtStatus(packet.status);
	if (error == ERROR_NOT_SUPPORTED && packet.status != STATUS_NOT_SUPPORTED)
		error = ntdll::RtlNtStatusToDosError(packet.status);
	setLastError(error);
	return FALSE;
}
} // namespace kernel32
