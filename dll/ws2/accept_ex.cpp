#include "mswsock.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "modules.h"
#include "ws2/async_io.h"
#include "ws2/internal.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#include <utility>

namespace {
struct AcceptRequest : ws2::detail::SocketIoRequest {
	std::shared_ptr<ws2::detail::Socket> accepted;
	uint8_t *buffer = nullptr;
	DWORD receiveLength = 0, localLength = 0, remoteLength = 0;
	bool connected = false;
	bool initialized = false;
	~AcceptRequest() override { release(); }
	[[nodiscard]] int descriptor() const override { return connected ? accepted->descriptor : socket->descriptor; }
	[[nodiscard]] short events() const override { return POLLIN; }
	[[nodiscard]] bool isCancelled() const override { return SocketIoRequest::isCancelled() || accepted->closed; }
	bool process() override;
	void release() override;
};

void AcceptRequest::release() {
	if (!std::exchange(initialized, false))
		return;
	{
		std::lock_guard lock(socket->ioMutex);
		if (--socket->pendingAccepts == 0) {
			if (!socket->closed)
				::fcntl(socket->descriptor, F_SETFL, socket->acceptOriginalFlags);
			socket->acceptOriginalFlags = -1;
		}
	}
	{
		std::lock_guard lock(accepted->ioMutex);
		accepted->acceptReserved = false;
		if (connected && status == STATUS_SUCCESS)
			accepted->acceptedByExtension = true;
	}
}

bool AcceptRequest::process() {
	if (!connected) {
		int descriptor;
		{
			std::lock_guard lock(socket->ioMutex);
			do {
				descriptor = ::accept(socket->descriptor, nullptr, nullptr);
			} while (descriptor < 0 && errno == EINTR);
		}
		if (descriptor < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return false;
			status = wibo::statusFromErrno(errno);
			return true;
		}
		{
			std::lock_guard lock(accepted->ioMutex);
			const int mode = ::fcntl(accepted->descriptor, F_GETFL);
			const int flags = ::fcntl(accepted->descriptor, F_GETFD);
			if (accepted->closed) {
				::close(descriptor);
				status = STATUS_CANCELLED;
				return true;
			}
			if (mode < 0 || flags < 0 || !ws2::detail::copySocketOptions(accepted->descriptor, descriptor) ||
				::dup2(descriptor, accepted->descriptor) < 0 || ::fcntl(accepted->descriptor, F_SETFL, mode) < 0 ||
				::fcntl(accepted->descriptor, F_SETFD, flags) < 0) {
				const int error = errno;
				::close(descriptor);
				status = wibo::statusFromErrno(error);
				return true;
			}
			::close(descriptor);
			connected = true;
		}
		for (unsigned side = 0; side < 2; ++side) {
			sockaddr_storage address{};
			socklen_t length = sizeof(address);
			const int result =
				side ? ::getpeername(accepted->descriptor, reinterpret_cast<sockaddr *>(&address), &length)
					 : ::getsockname(accepted->descriptor, reinterpret_cast<sockaddr *>(&address), &length);
			if (result < 0) {
				status = wibo::statusFromErrno(errno);
				return true;
			}
			auto *output = buffer + receiveLength + (side ? localLength : 0);
			int capacity = accepted->family == AF_INET ? 16 : 28;
			const int error =
				ws2::detail::addressFromNative(reinterpret_cast<const sockaddr *>(&address), output + 16, &capacity);
			if (error) {
				status = STATUS_INVALID_PARAMETER;
				return true;
			}
			std::memcpy(output, &capacity, sizeof(capacity));
		}
	}
	if (!receiveLength)
		return true;
	ssize_t received;
	do {
		received = ::recv(accepted->descriptor, buffer, receiveLength, MSG_DONTWAIT);
	} while (received < 0 && errno == EINTR);
	if (received < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return false;
		status = wibo::statusFromErrno(errno);
	} else {
		bytes = static_cast<DWORD>(received);
	}
	return true;
}

} // namespace

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
	request->socket = listener;
	request->accepted = accepted;
	request->initialized = true;
	request->binding = std::atomic_load(&listener->completion);
	request->overlapped = overlapped;
	request->buffer = static_cast<uint8_t *>(output);
	request->receiveLength = receiveLength;
	request->localLength = localLength;
	request->remoteLength = remoteLength;
	if (!ws2::detail::queueSocketIo(std::move(request))) {
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
