#include "mswsock.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/overlapped_util.h"
#include "modules.h"
#include "ws2/internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr NTSTATUS kCancelled = static_cast<NTSTATUS>(0xC0000120);
struct AcceptRequest {
	std::shared_ptr<ws2::detail::Socket> listener, accepted;
	std::shared_ptr<const kernel32::CompletionBinding> binding;
	OVERLAPPED *overlapped = nullptr;
	uint8_t *buffer = nullptr;
	DWORD receiveLength = 0, localLength = 0, remoteLength = 0;
	pthread_t owner = pthread_self();
	bool connected = false;
	bool cancelled = false;
	NTSTATUS status = STATUS_SUCCESS;
	DWORD bytes = 0;
	[[nodiscard]] int descriptor() const { return connected ? accepted->descriptor : listener->descriptor; }
};

void releaseRequest(AcceptRequest &request) {
	{
		std::lock_guard lock(request.listener->ioMutex);
		if (--request.listener->pendingAccepts == 0) {
			if (!request.listener->closed)
				::fcntl(request.listener->descriptor, F_SETFL, request.listener->acceptOriginalFlags);
			request.listener->acceptOriginalFlags = -1;
		}
	}
	{
		std::lock_guard lock(request.accepted->ioMutex);
		request.accepted->acceptReserved = false;
		if (request.connected && request.status == STATUS_SUCCESS)
			request.accepted->acceptedByExtension = true;
	}
}

void finish(AcceptRequest &request) {
	releaseRequest(request);
	kernel32::detail::signalOverlappedCompletion(request.binding, request.overlapped, request.status, request.bytes);
}

bool process(AcceptRequest &request) {
	if (request.cancelled || request.listener->closed || request.accepted->closed) {
		request.status = kCancelled;
		return true;
	}
	if (!request.connected) {
		int descriptor;
		{
			std::lock_guard lock(request.listener->ioMutex);
			do {
				descriptor = ::accept(request.listener->descriptor, nullptr, nullptr);
			} while (descriptor < 0 && errno == EINTR);
		}
		if (descriptor < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return false;
			request.status = wibo::statusFromErrno(errno);
			return true;
		}
		{
			std::lock_guard lock(request.accepted->ioMutex);
			const int mode = ::fcntl(request.accepted->descriptor, F_GETFL);
			const int flags = ::fcntl(request.accepted->descriptor, F_GETFD);
			if (request.accepted->closed) {
				::close(descriptor);
				request.status = kCancelled;
				return true;
			}
			if (mode < 0 || flags < 0 || !ws2::detail::copySocketOptions(request.accepted->descriptor, descriptor) ||
				::dup2(descriptor, request.accepted->descriptor) < 0 ||
				::fcntl(request.accepted->descriptor, F_SETFL, mode) < 0 ||
				::fcntl(request.accepted->descriptor, F_SETFD, flags) < 0) {
				const int error = errno;
				::close(descriptor);
				request.status = wibo::statusFromErrno(error);
				return true;
			}
			::close(descriptor);
			request.connected = true;
		}
		for (unsigned side = 0; side < 2; ++side) {
			sockaddr_storage address{};
			socklen_t length = sizeof(address);
			const int result =
				side ? ::getpeername(request.accepted->descriptor, reinterpret_cast<sockaddr *>(&address), &length)
					 : ::getsockname(request.accepted->descriptor, reinterpret_cast<sockaddr *>(&address), &length);
			if (result < 0) {
				request.status = wibo::statusFromErrno(errno);
				return true;
			}
			auto *output = request.buffer + request.receiveLength + (side ? request.localLength : 0);
			int capacity = request.accepted->family == AF_INET ? 16 : 28;
			const int error =
				ws2::detail::addressFromNative(reinterpret_cast<const sockaddr *>(&address), output + 16, &capacity);
			if (error) {
				request.status = STATUS_INVALID_PARAMETER;
				return true;
			}
			std::memcpy(output, &capacity, sizeof(capacity));
		}
	}
	if (!request.receiveLength)
		return true;
	ssize_t received;
	do {
		received = ::recv(request.accepted->descriptor, request.buffer, request.receiveLength, MSG_DONTWAIT);
	} while (received < 0 && errno == EINTR);
	if (received < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return false;
		request.status = wibo::statusFromErrno(errno);
	} else {
		request.bytes = static_cast<DWORD>(received);
	}
	return true;
}

class AcceptWorker;
std::atomic<AcceptWorker *> g_worker = nullptr;
class AcceptWorker {
	std::mutex mutex;
	std::vector<std::unique_ptr<AcceptRequest>> requests;
	std::atomic<bool> stopping = false;
	int wakePipe[2] = {-1, -1};
	std::thread thread;

	void run() {
		while (true) {
			std::vector<pollfd> descriptors;
			std::vector<AcceptRequest *> snapshot;
			bool immediate = stopping.load();
			{
				std::lock_guard lock(mutex);
				descriptors.push_back({wakePipe[0], POLLIN, 0});
				for (const auto &request : requests) {
					immediate |= request->cancelled || request->listener->closed || request->accepted->closed;
					snapshot.push_back(request.get());
					descriptors.push_back({request->descriptor(), POLLIN, 0});
				}
			}
			int ready;
			do {
				ready = ::poll(descriptors.data(), descriptors.size(), immediate ? 0 : -1);
			} while (ready < 0 && errno == EINTR);
			if (descriptors[0].revents) {
				std::array<char, 128> bytes{};
				while (::read(wakePipe[0], bytes.data(), bytes.size()) > 0) {
				}
			}
			std::vector<std::unique_ptr<AcceptRequest>> completed;
			{
				std::lock_guard lock(mutex);
				for (size_t i = 0; i < snapshot.size(); ++i) {
					auto *request = snapshot[i];
					request->cancelled |= stopping.load();
					if (descriptors[i + 1].revents || request->cancelled || request->listener->closed ||
						request->accepted->closed) {
						if (process(*request)) {
							auto found = std::find_if(requests.begin(), requests.end(),
													  [=](const auto &entry) { return entry.get() == request; });
							completed.push_back(std::move(*found));
							requests.erase(found);
						}
					}
				}
			}
			for (const auto &request : completed)
				finish(*request);
			if (stopping) {
				std::lock_guard lock(mutex);
				if (requests.empty())
					break;
			}
		}
	}

  public:
	AcceptWorker() {
		if (::pipe(wakePipe) < 0)
			return;
		for (int descriptor : wakePipe) {
			if (::fcntl(descriptor, F_SETFL, O_NONBLOCK) < 0 || ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
				::close(wakePipe[0]);
				::close(wakePipe[1]);
				wakePipe[0] = wakePipe[1] = -1;
				return;
			}
		}
		thread = std::thread(&AcceptWorker::run, this);
	}
	~AcceptWorker() {
		g_worker = nullptr;
		stopping = true;
		wake();
		if (thread.joinable())
			thread.join();
		for (int descriptor : wakePipe)
			if (descriptor >= 0)
				::close(descriptor);
	}
	bool enqueue(std::unique_ptr<AcceptRequest> &request) {
		std::lock_guard lock(mutex);
		if (!thread.joinable() || stopping || requests.size() >= 4096)
			return false;
		request->overlapped->Internal = STATUS_PENDING;
		request->overlapped->InternalHigh = 0;
		kernel32::detail::resetOverlappedEvent(request->overlapped);
		requests.push_back(std::move(request));
		wake();
		return true;
	}
	void wake() const {
		const char byte = 1;
		if (wakePipe[1] >= 0)
			while (::write(wakePipe[1], &byte, 1) < 0 && errno == EINTR) {
			}
	}
	void cancelThread(pthread_t owner) {
		std::lock_guard lock(mutex);
		for (const auto &request : requests)
			if (pthread_equal(owner, request->owner))
				request->cancelled = true;
		wake();
	}
};
AcceptWorker &worker() {
	static AcceptWorker instance;
	g_worker = &instance;
	return instance;
}
} // namespace

namespace ws2::detail {
void wakeSocketIo() {
	if (auto *worker = g_worker.load())
		worker->wake();
}
void cancelSocketIoForThread(pthread_t owner) {
	if (auto *worker = g_worker.load())
		worker->cancelThread(owner);
}
} // namespace ws2::detail

namespace mswsock {
BOOL WINAPI AcceptEx(SOCKET listenerHandle, SOCKET acceptedHandle, LPVOID output, DWORD receiveLength,
					 DWORD localLength, DWORD remoteLength, LPDWORD received, LPOVERLAPPED overlapped) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("AcceptEx(0x%llx, 0x%llx, %p, %u, %u, %u, %p, %p)\n", static_cast<unsigned long long>(listenerHandle),
			  static_cast<unsigned long long>(acceptedHandle), output, receiveLength, localLength, remoteLength,
			  received, overlapped);
	const auto listener = ws2::detail::findSocket(listenerHandle);
	const auto accepted = ws2::detail::findSocket(acceptedHandle);
	if (!listener || !accepted)
		return FALSE;
	auto fail = [](int error) {
		ws2::detail::setLastError(error);
		return FALSE;
	};
	const DWORD addressSize = listener->family == AF_INET ? 16 : 28;
	if (!output || !received || !overlapped)
		return fail(10014);
	if (listener == accepted || !listener->listening || listener->family != accepted->family ||
		localLength < addressSize + 16 || remoteLength < addressSize + 16 ||
		static_cast<uint64_t>(receiveLength) + localLength + remoteLength > UINT32_MAX)
		return fail(10022);
	if (!listener->overlapped || !accepted->overlapped)
		return fail(10045);
	{
		std::lock_guard lock(accepted->ioMutex);
		if (accepted->acceptReserved || accepted->closed)
			return fail(10022);
		int type = 0;
		socklen_t typeLength = sizeof(type);
		if (::getsockopt(accepted->descriptor, SOL_SOCKET, SO_TYPE, &type, &typeLength) < 0)
			return fail(ws2::detail::socketError(errno));
		if (type != SOCK_STREAM)
			return fail(10022);
		sockaddr_storage address{};
		socklen_t length = sizeof(address);
		if (::getsockname(accepted->descriptor, reinterpret_cast<sockaddr *>(&address), &length) < 0)
			return fail(ws2::detail::socketError(errno));
		const auto port = address.ss_family == AF_INET ? reinterpret_cast<const sockaddr_in *>(&address)->sin_port
													   : reinterpret_cast<const sockaddr_in6 *>(&address)->sin6_port;
		if (port)
			return fail(10022);
		accepted->acceptReserved = true;
	}
	{
		std::lock_guard lock(listener->ioMutex);
		if (!listener->pendingAccepts) {
			listener->acceptOriginalFlags = ::fcntl(listener->descriptor, F_GETFL);
			if (listener->acceptOriginalFlags < 0 ||
				::fcntl(listener->descriptor, F_SETFL, listener->acceptOriginalFlags | O_NONBLOCK) < 0) {
				std::lock_guard acceptedLock(accepted->ioMutex);
				accepted->acceptReserved = false;
				return fail(ws2::detail::socketError(errno));
			}
		}
		++listener->pendingAccepts;
	}
	auto request = std::make_unique<AcceptRequest>();
	request->listener = listener;
	request->accepted = accepted;
	request->binding = std::atomic_load(&listener->completion);
	request->overlapped = overlapped;
	request->buffer = static_cast<uint8_t *>(output);
	request->receiveLength = receiveLength;
	request->localLength = localLength;
	request->remoteLength = remoteLength;
	if (!worker().enqueue(request)) {
		releaseRequest(*request);
		return fail(10055);
	}
	return fail(ERROR_IO_PENDING);
}

void WINAPI GetAcceptExSockaddrs(LPVOID output, DWORD receiveLength, DWORD localLength, DWORD remoteLength,
								 GUEST_PTR *local, int *localSize, GUEST_PTR *remote, int *remoteSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetAcceptExSockaddrs(%p, %u, %u, %u, %p, %p, %p, %p)\n", output, receiveLength, localLength,
			  remoteLength, local, localSize, remote, remoteSize);
	if (!output || !local || !localSize || !remote || !remoteSize || localLength < 16 || remoteLength < 16)
		return;
	auto *buffer = static_cast<uint8_t *>(output) + receiveLength;
	std::memcpy(localSize, buffer, sizeof(*localSize));
	std::memcpy(remoteSize, buffer + localLength, sizeof(*remoteSize));
	*local = toGuestPtr(buffer + 16);
	*remote = toGuestPtr(buffer + localLength + 16);
}
} // namespace mswsock

#include "mswsock_trampolines.h"

extern const wibo::ModuleStub lib_mswsock = {
	(const char *[]){"mswsock", nullptr},
	mswsockThunkByName,
	nullptr,
};

namespace ws2 {
const wibo::ModuleStub &extensionModule() { return lib_mswsock; }
} // namespace ws2
