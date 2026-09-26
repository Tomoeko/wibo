#include "ws2/async_io.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/overlapped_util.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

namespace {
NTSTATUS socketStatus(int error) {
	if (error == ECONNRESET || error == EPIPE)
		return STATUS_CONNECTION_RESET;
	return wibo::statusFromErrno(error);
}
int socketResult(NTSTATUS status) {
	switch (status) {
	case STATUS_SUCCESS:
		return 0;
	case STATUS_PENDING:
		return ERROR_IO_INCOMPLETE;
	case STATUS_CONNECTION_RESET:
		return 10054;
	case STATUS_BUFFER_OVERFLOW:
		return 10040;
	case STATUS_INVALID_PARAMETER:
		return 10022;
	case STATUS_INVALID_HANDLE:
		return 10038;
	case STATUS_NOT_SUPPORTED:
		return 10045;
	default:
		return static_cast<int>(wibo::winErrorFromNtStatus(status));
	}
}
struct TransferRequest final : ws2::detail::SocketIoRequest {
	std::vector<iovec> buffers;
	bool sending = false;
	bool stream = false;
	int flags = 0;
	uint32_t remaining = 0;
	bool countedSend = false;
	~TransferRequest() override { release(); }
	void release() override {
		if (countedSend) {
			countedSend = false;
			ws2::detail::finishSocketSend(*socket);
		}
	}
	[[nodiscard]] short events() const override { return sending ? POLLOUT : POLLIN; }
	bool process() override {
		ssize_t transferred;
		msghdr message{};
		message.msg_iov = buffers.data();
		message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(buffers.size());
		std::lock_guard lock(socket->ioMutex);
		do {
			if (sending) {
				int nativeFlags = flags | MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
				nativeFlags |= MSG_NOSIGNAL;
#endif
				transferred = ::sendmsg(socket->descriptor, &message, nativeFlags);
			} else if (stream && !remaining) {
				char byte;
				transferred = ::recv(socket->descriptor, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
				if (transferred >= 0)
					transferred = 0;
			} else {
				transferred = ::recvmsg(socket->descriptor, &message, flags | MSG_DONTWAIT);
			}
		} while (transferred < 0 && errno == EINTR);
		if (transferred < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return false;
			status = socketStatus(errno);
			return true;
		}
		bytes += static_cast<DWORD>(transferred);
		if (!sending) {
			if (message.msg_flags & MSG_TRUNC)
				status = STATUS_BUFFER_OVERFLOW;
			return true;
		}
		if (!stream || static_cast<uint32_t>(transferred) == remaining)
			return true;
		if (!transferred) {
			status = STATUS_UNEXPECTED_IO_ERROR;
			return true;
		}
		remaining -= static_cast<uint32_t>(transferred);
		size_t consumed = static_cast<size_t>(transferred);
		for (auto &buffer : buffers) {
			const size_t amount = std::min(consumed, buffer.iov_len);
			if (amount)
				buffer.iov_base = static_cast<uint8_t *>(buffer.iov_base) + amount;
			buffer.iov_len -= amount;
			consumed -= amount;
			if (!consumed)
				break;
		}
		return false;
	}
};

int transfer(SOCKET handle, const WSABUF *buffers, DWORD count, LPDWORD transferred, DWORD flags, LPDWORD returnedFlags,
			 OVERLAPPED *overlapped, GUEST_PTR completionRoutine, bool sending) {
	const auto socket = ws2::detail::findSocket(handle);
	if (!socket)
		return -1;
	if (sending ? socket->sendShutdown.load() : socket->receiveShutdown.load())
		return ws2::detail::failSocket(10058);
	const bool asynchronous = overlapped && socket->overlapped;
	if (!buffers || (!sending && !returnedFlags) || (!asynchronous && !transferred))
		return ws2::detail::failSocket(10014);
	const long maximum = ::sysconf(_SC_IOV_MAX);
	if (!count || count > static_cast<uint32_t>(maximum > 0 ? maximum : IOV_MAX) || (flags & ~(sending ? 5U : 0xBU)))
		return ws2::detail::failSocket(10022);
	if ((socket->overlapped && completionRoutine) || (asynchronous && !sending && flags))
		return ws2::detail::failSocket(10045);
	auto request = std::make_unique<TransferRequest>();
	request->socket = socket;
	request->binding = std::atomic_load(&socket->completion);
	request->overlapped = overlapped;
	request->sending = sending;
	request->order = sending ? ws2::detail::SocketIoOrder::Send : ws2::detail::SocketIoOrder::Receive;
	uint64_t total = 0;
	for (DWORD i = 0; i < count; ++i) {
		if (!buffers[i].buf && buffers[i].len)
			return ws2::detail::failSocket(10014);
		total += buffers[i].len;
		if (total > UINT32_MAX)
			return ws2::detail::failSocket(10022);
		request->buffers.push_back({fromGuestPtr<void>(buffers[i].buf), buffers[i].len});
	}
	request->remaining = static_cast<uint32_t>(total);
	int type = 0;
	socklen_t typeLength = sizeof(type);
	if (::getsockopt(socket->descriptor, SOL_SOCKET, SO_TYPE, &type, &typeLength) < 0)
		return ws2::detail::failSocket(ws2::detail::socketError(errno));
	request->stream = type == SOCK_STREAM;
	if ((flags & 1) && !request->stream)
		return ws2::detail::failSocket(10045);
	if (request->stream) {
		sockaddr_storage peer{};
		socklen_t length = sizeof(peer);
		if (::getpeername(socket->descriptor, reinterpret_cast<sockaddr *>(&peer), &length) < 0)
			return ws2::detail::failSocket(10057);
	} else if (!sending) {
		sockaddr_storage address{};
		socklen_t length = sizeof(address);
		if (::getsockname(socket->descriptor, reinterpret_cast<sockaddr *>(&address), &length) < 0)
			return ws2::detail::failSocket(ws2::detail::socketError(errno));
		const auto port = address.ss_family == AF_INET ? reinterpret_cast<const sockaddr_in *>(&address)->sin_port
													   : reinterpret_cast<const sockaddr_in6 *>(&address)->sin6_port;
		if (!port)
			return ws2::detail::failSocket(10022);
	}
	request->flags = ((flags & 1) ? MSG_OOB : 0) | ((flags & 2) ? MSG_PEEK : 0) | ((flags & 4) ? MSG_DONTROUTE : 0) |
					 ((flags & 8) ? MSG_WAITALL : 0);
	if (asynchronous) {
		if (sending) {
			std::lock_guard lock(socket->ioMutex);
			if (socket->sendShutdown)
				return ws2::detail::failSocket(10058);
			++socket->pendingSends;
			request->countedSend = true;
		}
		if (!ws2::detail::queueSocketIo(std::move(request)))
			return ws2::detail::failSocket(10055);
		return ws2::detail::failSocket(ERROR_IO_PENDING);
	}
	msghdr message{};
	message.msg_iov = request->buffers.data();
	message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(request->buffers.size());
	ssize_t result;
	do {
		if (sending) {
			int nativeFlags = request->flags;
#ifdef MSG_NOSIGNAL
			nativeFlags |= MSG_NOSIGNAL;
#endif
			result = ::sendmsg(socket->descriptor, &message, nativeFlags);
		} else if (request->stream && !total) {
			char byte;
			result = ::recv(socket->descriptor, &byte, 1, MSG_PEEK);
			if (result >= 0)
				result = 0;
		} else {
			result = ::recvmsg(socket->descriptor, &message, request->flags);
		}
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		return ws2::detail::failSocket(ws2::detail::socketError(errno));
	if (transferred)
		*transferred = static_cast<DWORD>(result);
	if (returnedFlags)
		*returnedFlags = 0;
	return (message.msg_flags & MSG_TRUNC) ? ws2::detail::failSocket(10040) : 0;
}
} // namespace

namespace ws2 {
int WINAPI WSARecv(SOCKET handle, const WSABUF *buffers, DWORD count, LPDWORD received, LPDWORD flags,
				   LPOVERLAPPED overlapped, GUEST_PTR completionRoutine) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSARecv(0x%llx, %p, %u, %p, %p, %p, %p)\n", static_cast<unsigned long long>(handle), buffers, count,
			  received, flags, overlapped, fromGuestPtr<void>(completionRoutine));
	return transfer(handle, buffers, count, received, flags ? *flags : 0, flags, overlapped, completionRoutine, false);
}
int WINAPI WSASend(SOCKET handle, const WSABUF *buffers, DWORD count, LPDWORD sent, DWORD flags,
				   LPOVERLAPPED overlapped, GUEST_PTR completionRoutine) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSASend(0x%llx, %p, %u, %p, 0x%x, %p, %p)\n", static_cast<unsigned long long>(handle), buffers, count,
			  sent, flags, overlapped, fromGuestPtr<void>(completionRoutine));
	return transfer(handle, buffers, count, sent, flags, nullptr, overlapped, completionRoutine, true);
}
BOOL WINAPI WSAGetOverlappedResult(SOCKET handle, LPOVERLAPPED overlapped, LPDWORD transferred, BOOL wait,
								   LPDWORD flags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSAGetOverlappedResult(0x%llx, %p, %p, %d, %p)\n", static_cast<unsigned long long>(handle), overlapped,
			  transferred, wait, flags);
	const auto socket = detail::findSocket(handle);
	if (!socket)
		return FALSE;
	if (!overlapped || !transferred || !flags) {
		detail::setLastError(10014);
		return FALSE;
	}
	auto status = [&] { return kernel32::detail::loadOverlappedStatus(overlapped); };
	if (wait && status() == STATUS_PENDING) {
		std::unique_lock lock(socket->ioMutex);
		kernel32::CompletionWait completionWait;
		socket->overlappedCv.wait(lock, [&] { return status() != STATUS_PENDING; });
	}
	const int error = socketResult(status());
	if (error) {
		detail::setLastError(error);
		return FALSE;
	}
	*transferred = static_cast<DWORD>(kernel32::detail::loadOverlappedBytes(overlapped));
	*flags = 0;
	return TRUE;
}
} // namespace ws2
