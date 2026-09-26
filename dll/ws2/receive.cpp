#include "ws2/internal.h"

#include "common.h"
#include "context.h"

#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace ws2 {
namespace {
int receive(SOCKET handle, LPSTR buffer, int length, int flags, LPVOID address, int *addressLength) {
	const auto state = detail::findSocket(handle);
	if (!state)
		return -1;
	if (length < 0 || (flags & ~0xBU))
		return detail::failSocket(10022);
	if (!buffer && length)
		return detail::failSocket(10014);
	int type = 0;
	socklen_t size = sizeof(type);
	if (::getsockopt(state->descriptor, SOL_SOCKET, SO_TYPE, &type, &size) < 0)
		return detail::failSocket(detail::socketError(errno));
	if (type == SOCK_STREAM && !length)
		return detail::failSocket(10022);
	const int mode = ::fcntl(state->descriptor, F_GETFL);
	if (mode < 0)
		return detail::failSocket(detail::socketError(errno));
	if ((flags & 8) && (type != SOCK_STREAM || (mode & O_NONBLOCK) || (flags & 3)))
		return detail::failSocket(10045);
	if ((flags & 1) && type != SOCK_STREAM)
		return detail::failSocket(10045);
	const bool captureAddress = type == SOCK_DGRAM && address;
	if (captureAddress && (!addressLength || *addressLength < (state->family == AF_INET ? 16 : 28)))
		return detail::failSocket(10014);
	if (type == SOCK_DGRAM) {
		sockaddr_storage local{};
		size = sizeof(local);
		if (::getsockname(state->descriptor, reinterpret_cast<sockaddr *>(&local), &size) < 0)
			return detail::failSocket(detail::socketError(errno));
		const auto port = local.ss_family == AF_INET ? reinterpret_cast<const sockaddr_in *>(&local)->sin_port
													 : reinterpret_cast<const sockaddr_in6 *>(&local)->sin6_port;
		if (!port)
			return detail::failSocket(10022);
	}
	const int nativeFlags =
		((flags & 1) ? MSG_OOB : 0) | ((flags & 2) ? MSG_PEEK : 0) | ((flags & 8) ? MSG_WAITALL : 0);
	ssize_t received;
	bool truncated = false;
	sockaddr_storage source{};
	do {
		if (type == SOCK_DGRAM) {
			iovec output{buffer, static_cast<size_t>(length)};
			msghdr message{};
			message.msg_iov = &output;
			message.msg_iovlen = 1;
			if (captureAddress) {
				message.msg_name = &source;
				message.msg_namelen = sizeof(source);
			}
			received = ::recvmsg(state->descriptor, &message, nativeFlags);
			truncated = message.msg_flags & MSG_TRUNC;
		} else
			received = ::recv(state->descriptor, buffer, length, nativeFlags);
	} while (received < 0 && errno == EINTR);
	if (received < 0) {
		const int error = errno;
		if (error == EAGAIN && !(mode & O_NONBLOCK)) {
			timeval timeout{};
			size = sizeof(timeout);
			if (::getsockopt(state->descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, &size) == 0 &&
				(timeout.tv_sec || timeout.tv_usec))
				return detail::failSocket(10060);
		}
		return detail::failSocket(detail::socketError(error));
	}
	if (captureAddress) {
		const auto error = detail::addressFromNative(reinterpret_cast<sockaddr *>(&source), address, addressLength);
		if (error)
			return detail::failSocket(error);
	}
	if (truncated)
		return detail::failSocket(10040);
	return static_cast<int>(received);
}
} // namespace

int WINAPI recv(SOCKET handle, LPSTR buffer, int length, int flags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("recv(0x%llx, %p, %d, %d)\n", static_cast<unsigned long long>(handle), buffer, length, flags);
	return receive(handle, buffer, length, flags, nullptr, nullptr);
}

int WINAPI recvfrom(SOCKET handle, LPSTR buffer, int length, int flags, LPVOID address, int *addressLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("recvfrom(0x%llx, %p, %d, %d, %p, %p)\n", static_cast<unsigned long long>(handle), buffer, length, flags,
			  address, addressLength);
	return receive(handle, buffer, length, flags, address, addressLength);
}
int WINAPI send(SOCKET handle, LPCSTR buffer, int length, int flags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("send(0x%llx, %p, %d, %d)\n", static_cast<unsigned long long>(handle), buffer, length, flags);
	const auto state = detail::findSocket(handle);
	if (!state)
		return -1;
	if (length < 0 || (flags & ~5U))
		return detail::failSocket(10022);
	if (!buffer && length)
		return detail::failSocket(10014);
	int nativeFlags = ((flags & 1) ? MSG_OOB : 0) | ((flags & 4) ? MSG_DONTROUTE : 0);
#ifdef MSG_NOSIGNAL
	nativeFlags |= MSG_NOSIGNAL;
#endif
	ssize_t sent;
	do {
		sent = ::send(state->descriptor, buffer, length, nativeFlags);
	} while (sent < 0 && errno == EINTR);
	return sent < 0 ? detail::failSocket(detail::socketError(errno)) : static_cast<int>(sent);
}
} // namespace ws2
