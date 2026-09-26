#include "ws2/internal.h"

#include "common.h"
#include "context.h"
#include "handles.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/time.h>
#include <unistd.h>
#include <unordered_map>

namespace {
constexpr SOCKET kInvalidSocket = static_cast<SOCKET>(-1);
constexpr int kWinInet6 = 23;
int querySocketAddress(SOCKET handle, LPVOID address, int *length, bool peer) {
	const auto state = ws2::detail::findSocket(handle);
	if (!state)
		return -1;
	sockaddr_storage native{};
	socklen_t nativeLength = sizeof(native);
	const int result = peer ? ::getpeername(state->descriptor, reinterpret_cast<sockaddr *>(&native), &nativeLength)
							: ::getsockname(state->descriptor, reinterpret_cast<sockaddr *>(&native), &nativeLength);
	if (result < 0) {
		const int error = errno;
		return ws2::detail::failSocket(peer && error == EINVAL ? 10057 : ws2::detail::socketError(error));
	}
	if (!peer && reinterpret_cast<const sockaddr_in *>(&native)->sin_port == 0)
		return ws2::detail::failSocket(10022);
	const int status = ws2::detail::addressFromNative(reinterpret_cast<sockaddr *>(&native), address, length);
	return status ? ws2::detail::failSocket(status) : 0;
}
struct SocketRegistry {
	std::mutex mutex;
	std::unordered_map<SOCKET, std::shared_ptr<ws2::detail::Socket>> sockets;
	SOCKET next = 0x40000000;
};
SocketRegistry &socketRegistry() {
	static SocketRegistry registry;
	return registry;
}

SOCKET registerSocket(std::shared_ptr<ws2::detail::Socket> state) {
	using namespace ws2::detail;
	auto &registry = socketRegistry();
	std::lock_guard lock(registry.mutex);
	if (!requireStarted())
		return kInvalidSocket;
	if (registry.next == kInvalidSocket) {
		setLastError(10024);
		return kInvalidSocket;
	}
	const SOCKET handle = registry.next++;
	registry.sockets.emplace(handle, std::move(state));
	return handle;
}

SOCKET createSocket(int family, int type, int protocol, LPCVOID protocolInfo, UINT group, DWORD flags) {
	using namespace ws2::detail;
	if (!requireStarted())
		return kInvalidSocket;
	auto fail = [](int error) {
		setLastError(error);
		return kInvalidSocket;
	};
	if (protocolInfo || group)
		return fail(10045);
	if (flags & ~0x81U)
		return fail(10022);
	if (!family)
		family = AF_INET;
	if (family != AF_INET && family != kWinInet6)
		return fail(10047);
	if (type != SOCK_STREAM && type != SOCK_DGRAM)
		return fail(10044);
	if (protocol != 0 && protocol != (type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP))
		return fail(10043);
	const int descriptor = ::socket(family == kWinInet6 ? AF_INET6 : AF_INET, type, protocol);
	if (descriptor < 0)
		return fail(socketError(errno));
	auto state = std::make_shared<Socket>(descriptor, family);
	state->overlapped = (flags & 1U) != 0;
	state->handleFlags = (flags & 0x80U) ? 0 : HANDLE_FLAG_INHERIT;
	if ((flags & 0x80U) && ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0)
		return fail(socketError(errno));
#if defined(__APPLE__)
	const int noSignal = 1;
	if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal)) < 0)
		return fail(socketError(errno));
#endif
	// Match the default IPv6-only mode of the Windows Internet provider.
	if (family == kWinInet6) {
		const int v6Only = 1;
		if (::setsockopt(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, &v6Only, sizeof(v6Only)) < 0)
			return fail(socketError(errno));
	}
	return registerSocket(std::move(state));
}
} // namespace

namespace ws2::detail {
bool copySocketOptions(int source, int destination) {
	constexpr int options[] = {SO_REUSEADDR, SO_KEEPALIVE, SO_DONTROUTE, SO_BROADCAST, SO_OOBINLINE,
							   SO_SNDBUF,	 SO_RCVBUF,	   SO_SNDTIMEO,	 SO_RCVTIMEO,  SO_LINGER};
	for (int option : options) {
		std::array<uint8_t, std::max(sizeof(timeval), sizeof(linger))> value{};
		socklen_t length = value.size();
		if (::getsockopt(source, SOL_SOCKET, option, value.data(), &length) < 0)
			return false;
#ifdef __linux__
		if (option == SO_SNDBUF || option == SO_RCVBUF) {
			int size;
			std::memcpy(&size, value.data(), sizeof(size));
			size /= 2;
			std::memcpy(value.data(), &size, sizeof(size));
		}
#endif
		if (::setsockopt(destination, SOL_SOCKET, option, value.data(), length) < 0)
			return false;
	}
	int noDelay = 0;
	socklen_t length = sizeof(noDelay);
	if (::getsockopt(source, IPPROTO_TCP, TCP_NODELAY, &noDelay, &length) < 0 ||
		::setsockopt(destination, IPPROTO_TCP, TCP_NODELAY, &noDelay, length) < 0)
		return false;
	return true;
}
Socket::~Socket() { ::close(descriptor); }
void finishSocketSend(Socket &socket) {
	std::lock_guard lock(socket.ioMutex);
	if (--socket.pendingSends == 0 && socket.sendShutdown && !socket.nativeSendShutdown && !socket.closed) {
		if (::shutdown(socket.descriptor, SHUT_WR) < 0)
			DEBUG_LOG("Deferred socket shutdown failed: %d\n", errno);
		socket.nativeSendShutdown = true;
	}
}
std::shared_ptr<Socket> findSocket(SOCKET handle) {
	if (!requireStarted())
		return nullptr;
	auto &registry = socketRegistry();
	std::lock_guard lock(registry.mutex);
	const auto found = registry.sockets.find(handle);
	if (found == registry.sockets.end()) {
		setLastError(10038);
		return nullptr;
	}
	return found->second;
}
bool getHandleInformation(SOCKET handle, DWORD *flags) {
	auto &registry = socketRegistry();
	std::lock_guard lock(registry.mutex);
	const auto found = registry.sockets.find(handle);
	if (found == registry.sockets.end())
		return false;
	*flags = found->second->handleFlags;
	return true;
}

bool setHandleInformation(SOCKET handle, DWORD mask, DWORD flags) {
	auto &registry = socketRegistry();
	std::lock_guard lock(registry.mutex);
	const auto found = registry.sockets.find(handle);
	if (found == registry.sockets.end())
		return false;
	mask &= HANDLE_FLAG_INHERIT | HANDLE_FLAG_PROTECT_FROM_CLOSE;
	const DWORD updated = (found->second->handleFlags & ~mask) | (flags & mask);
	if (mask & HANDLE_FLAG_INHERIT) {
		std::lock_guard socketLock(found->second->ioMutex);
		const int descriptor = found->second->descriptor;
		const int current = ::fcntl(descriptor, F_GETFD);
		if (current < 0 || ::fcntl(descriptor, F_SETFD,
								   updated & HANDLE_FLAG_INHERIT ? current & ~FD_CLOEXEC : current | FD_CLOEXEC) < 0)
			return false;
	}
	found->second->handleFlags = updated;
	return true;
}

void cleanupSockets() {
	auto &registry = socketRegistry();
	std::lock_guard lock(registry.mutex);
	for (const auto &[handle, state] : registry.sockets) {
		std::lock_guard socketLock(state->ioMutex);
		state->closed = true;
		::shutdown(state->descriptor, SHUT_RDWR);
	}
	registry.sockets.clear();
	wakeSocketIo();
}
int socketError(int error) {
	switch (error) {
	case EINTR:
		return 10004;
	case EACCES:
	case EPERM:
		return 10013;
	case EFAULT:
		return 10014;
	case EINVAL:
		return 10022;
	case EMFILE:
	case ENFILE:
		return 10024;
	case EAGAIN:
		return 10035;
	case EINPROGRESS:
		return 10036;
	case EALREADY:
		return 10037;
	case EBADF:
	case ENOTSOCK:
		return 10038;
	case EDESTADDRREQ:
		return 10039;
	case EMSGSIZE:
		return 10040;
	case EPROTOTYPE:
		return 10041;
	case ENOPROTOOPT:
		return 10042;
	case EPROTONOSUPPORT:
		return 10043;
	case ESOCKTNOSUPPORT:
		return 10044;
	case EOPNOTSUPP:
		return 10045;
	case EAFNOSUPPORT:
		return 10047;
	case EADDRINUSE:
		return 10048;
	case EADDRNOTAVAIL:
		return 10049;
	case ENETDOWN:
		return 10050;
	case ENETUNREACH:
		return 10051;
	case ENETRESET:
		return 10052;
	case ECONNABORTED:
		return 10053;
	case ECONNRESET:
	case EPIPE:
		return 10054;
	case ENOBUFS:
	case ENOMEM:
		return 10055;
	case EISCONN:
		return 10056;
	case ENOTCONN:
		return 10057;
	case ETIMEDOUT:
		return 10060;
	case ECONNREFUSED:
		return 10061;
	case EHOSTUNREACH:
		return 10065;
	default:
		return 10022;
	}
}
int failSocket(int error) {
	setLastError(error);
	return -1;
}
int addressToNative(LPCVOID address, int length, sockaddr_storage &result, socklen_t &resultLength) {
	if (!address || length < 2)
		return 10014;
	const auto *bytes = static_cast<const unsigned char *>(address);
	uint16_t family = 0;
	std::memcpy(&family, bytes, 2);
	std::memset(&result, 0, sizeof(result));
	if (family == AF_INET) {
		if (length < 16)
			return 10014;
		auto *ip = reinterpret_cast<sockaddr_in *>(&result);
		ip->sin_family = AF_INET;
		std::memcpy(&ip->sin_port, bytes + 2, 2);
		std::memcpy(&ip->sin_addr, bytes + 4, 4);
		resultLength = sizeof(*ip);
#if defined(__APPLE__)
		ip->sin_len = resultLength;
#endif
	} else if (family == kWinInet6) {
		if (length < 28)
			return 10014;
		auto *ip = reinterpret_cast<sockaddr_in6 *>(&result);
		ip->sin6_family = AF_INET6;
		std::memcpy(&ip->sin6_port, bytes + 2, 2);
		std::memcpy(&ip->sin6_flowinfo, bytes + 4, 4);
		std::memcpy(&ip->sin6_addr, bytes + 8, 16);
		std::memcpy(&ip->sin6_scope_id, bytes + 24, 4);
		resultLength = sizeof(*ip);
#if defined(__APPLE__)
		ip->sin6_len = resultLength;
#endif
	} else
		return 10047;
	return 0;
}
int addressFromNative(const sockaddr *address, LPVOID result, int *length) {
	if (!result || !length)
		return 10014;
	const int size = address->sa_family == AF_INET ? 16 : address->sa_family == AF_INET6 ? 28 : 0;
	if (!size)
		return 10047;
	if (*length < size)
		return 10014;
	std::array<unsigned char, 28> data{};
	const uint16_t family = address->sa_family == AF_INET ? AF_INET : kWinInet6;
	std::memcpy(data.data(), &family, 2);
	if (address->sa_family == AF_INET) {
		const auto *ip = reinterpret_cast<const sockaddr_in *>(address);
		std::memcpy(data.data() + 2, &ip->sin_port, 2);
		std::memcpy(data.data() + 4, &ip->sin_addr, 4);
	} else {
		const auto *ip = reinterpret_cast<const sockaddr_in6 *>(address);
		std::memcpy(data.data() + 2, &ip->sin6_port, 2);
		std::memcpy(data.data() + 4, &ip->sin6_flowinfo, 4);
		std::memcpy(data.data() + 8, &ip->sin6_addr, 16);
		std::memcpy(data.data() + 24, &ip->sin6_scope_id, 4);
	}
	std::memcpy(result, data.data(), size);
	*length = size;
	return 0;
}
} // namespace ws2::detail

namespace ws2 {
SOCKET WINAPI WSASocketA(int family, int type, int protocol, LPCVOID protocolInfo, UINT group, DWORD flags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSASocketA(%d, %d, %d, %p, %u, 0x%x)\n", family, type, protocol, protocolInfo, group, flags);
	return createSocket(family, type, protocol, protocolInfo, group, flags);
}
SOCKET WINAPI socket(int family, int type, int protocol) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("socket(%d, %d, %d)\n", family, type, protocol);
	return createSocket(family, type, protocol, nullptr, 0, 1);
}
int WINAPI closesocket(SOCKET handle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("closesocket(0x%llx)\n", static_cast<unsigned long long>(handle));
	if (!detail::requireStarted())
		return -1;
	auto &registry = socketRegistry();
	std::lock_guard lock(registry.mutex);
	const auto found = registry.sockets.find(handle);
	if (found == registry.sockets.end())
		return detail::failSocket(10038);
	{
		std::lock_guard socketLock(found->second->ioMutex);
		found->second->closed = true;
		::shutdown(found->second->descriptor, SHUT_RDWR);
	}
	registry.sockets.erase(found);
	detail::wakeSocketIo();
	return 0;
}
int WINAPI shutdown(SOCKET handle, int how) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("shutdown(0x%llx, %d)\n", static_cast<unsigned long long>(handle), how);
	const auto state = detail::findSocket(handle);
	if (!state)
		return -1;
	if (how < 0 || how > 2)
		return detail::failSocket(10022);
	{
		std::lock_guard lock(state->ioMutex);
		if (state->closed)
			return detail::failSocket(10038);
		int type = 0;
		socklen_t length = sizeof(type);
		if (::getsockopt(state->descriptor, SOL_SOCKET, SO_TYPE, &type, &length) < 0)
			return detail::failSocket(detail::socketError(errno));
		if (type == SOCK_STREAM && !state->sendShutdown) {
			sockaddr_storage peer{};
			length = sizeof(peer);
			if (::getpeername(state->descriptor, reinterpret_cast<sockaddr *>(&peer), &length) < 0) {
				const int error = errno;
				DEBUG_LOG("Socket shutdown peer query failed: %d\n", error);
				return detail::failSocket(detail::socketError(error));
			}
		}
		const bool receiving = how != 1;
		const bool sending = how != 0;
		const bool deferSend = sending && state->pendingSends != 0;
		if (receiving || !deferSend) {
			const int nativeHow = receiving ? (sending && !deferSend ? SHUT_RDWR : SHUT_RD) : SHUT_WR;
			if (::shutdown(state->descriptor, nativeHow) < 0 &&
				!(errno == ENOTCONN && (type == SOCK_DGRAM || state->sendShutdown))) {
				const int error = errno;
				DEBUG_LOG("Socket shutdown failed: %d\n", error);
				return detail::failSocket(detail::socketError(error));
			}
		}
		if (receiving)
			state->receiveShutdown = true;
		if (sending) {
			state->sendShutdown = true;
			state->nativeSendShutdown |= !deferSend;
		}
	}
	detail::wakeSocketIo();
	return 0;
}
int WINAPI bind(SOCKET handle, LPCVOID address, int length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("bind(0x%llx, %p, %d)\n", static_cast<unsigned long long>(handle), address, length);
	const auto state = detail::findSocket(handle);
	if (!state)
		return -1;
	sockaddr_storage native{};
	socklen_t nativeLength = 0;
	const int status = detail::addressToNative(address, length, native, nativeLength);
	if (status)
		return detail::failSocket(status);
	if (::bind(state->descriptor, reinterpret_cast<sockaddr *>(&native), nativeLength) < 0)
		return detail::failSocket(detail::socketError(errno));
	return 0;
}
int WINAPI connect(SOCKET handle, LPCVOID address, int length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("connect(0x%llx, %p, %d)\n", static_cast<unsigned long long>(handle), address, length);
	const auto state = detail::findSocket(handle);
	if (!state)
		return -1;
	sockaddr_storage native{};
	socklen_t nativeLength = 0;
	const int status = detail::addressToNative(address, length, native, nativeLength);
	if (status)
		return detail::failSocket(status);
	if ((native.ss_family == AF_INET ? AF_INET : kWinInet6) != state->family)
		return detail::failSocket(10047);
	int type = 0;
	socklen_t typeLength = sizeof(type);
	if (::getsockopt(state->descriptor, SOL_SOCKET, SO_TYPE, &type, &typeLength) < 0)
		return detail::failSocket(detail::socketError(errno));
	const bool unspecified = native.ss_family == AF_INET
								 ? reinterpret_cast<const sockaddr_in *>(&native)->sin_addr.s_addr == 0
								 : IN6_IS_ADDR_UNSPECIFIED(&reinterpret_cast<const sockaddr_in6 *>(&native)->sin6_addr);
	if (unspecified) {
		if (type != SOCK_DGRAM)
			return detail::failSocket(10049);
		native = {};
		native.ss_family = AF_UNSPEC;
		nativeLength = sizeof(sockaddr);
#if defined(__APPLE__)
		native.ss_len = nativeLength;
#endif
	}
	state->connecting.store(false);
	if (::connect(state->descriptor, reinterpret_cast<sockaddr *>(&native), nativeLength) < 0) {
		const int error = errno;
		state->connecting.store(error == EINPROGRESS || error == EALREADY);
		return detail::failSocket(error == EINPROGRESS ? 10035 : detail::socketError(error));
	}
	return 0;
}
int WINAPI listen(SOCKET handle, int backlog) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("listen(0x%llx, %d)\n", static_cast<unsigned long long>(handle), backlog);
	const auto state = detail::findSocket(handle);
	if (!state)
		return -1;
	int type = 0;
	socklen_t size = sizeof(type);
	if (::getsockopt(state->descriptor, SOL_SOCKET, SO_TYPE, &type, &size) < 0)
		return detail::failSocket(detail::socketError(errno));
	if (type != SOCK_STREAM)
		return detail::failSocket(10045);
	sockaddr_storage address{};
	size = sizeof(address);
	if (::getpeername(state->descriptor, reinterpret_cast<sockaddr *>(&address), &size) == 0)
		return detail::failSocket(10056);
	size = sizeof(address);
	if (::getsockname(state->descriptor, reinterpret_cast<sockaddr *>(&address), &size) < 0)
		return detail::failSocket(detail::socketError(errno));
	const auto port = address.ss_family == AF_INET ? reinterpret_cast<const sockaddr_in *>(&address)->sin_port
												   : reinterpret_cast<const sockaddr_in6 *>(&address)->sin6_port;
	if (!port)
		return detail::failSocket(10022);
	if (backlog < 0)
		backlog = static_cast<int>(std::clamp<int64_t>(-static_cast<int64_t>(backlog), 200, 65535));
	if (::listen(state->descriptor, backlog) < 0)
		return detail::failSocket(detail::socketError(errno));
	state->listening.store(true);
	return 0;
}

SOCKET WINAPI accept(SOCKET handle, LPVOID address, int *addressLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("accept(0x%llx, %p, %p)\n", static_cast<unsigned long long>(handle), address, addressLength);
	const auto state = detail::findSocket(handle);
	if (!state)
		return kInvalidSocket;
	auto fail = [](int error) {
		detail::setLastError(error);
		return kInvalidSocket;
	};
	int type = 0;
	socklen_t size = sizeof(type);
	if (::getsockopt(state->descriptor, SOL_SOCKET, SO_TYPE, &type, &size) < 0)
		return fail(detail::socketError(errno));
	if (type != SOCK_STREAM)
		return fail(10045);
	if (!state->listening.load())
		return fail(10022);
	if (address && (!addressLength || *addressLength < (state->family == AF_INET ? 16 : 28)))
		return fail(10014);
	sockaddr_storage peer{};
	int descriptor;
	while (true) {
		bool blocking;
		{
			std::unique_lock lock(state->ioMutex);
			const int flags = state->pendingAccepts ? state->acceptOriginalFlags : ::fcntl(state->descriptor, F_GETFL);
			blocking = flags >= 0 && !(flags & O_NONBLOCK);
			if (!state->pendingAccepts)
				lock.unlock();
			size = sizeof(peer);
			descriptor = ::accept(state->descriptor, reinterpret_cast<sockaddr *>(&peer), &size);
		}
		if (descriptor >= 0)
			break;
		if (errno == EINTR)
			continue;
		if ((errno != EAGAIN && errno != EWOULDBLOCK) || !blocking || state->closed)
			break;
		pollfd ready{state->descriptor, POLLIN, 0};
		while (::poll(&ready, 1, -1) < 0 && errno == EINTR) {
		}
	}
	if (descriptor < 0)
		return fail(detail::socketError(errno));
	auto accepted = std::make_shared<detail::Socket>(descriptor, state->family);
	accepted->overlapped = state->overlapped;
	const int parentMode = [&] {
		std::lock_guard socketLock(state->ioMutex);
		return state->pendingAccepts ? state->acceptOriginalFlags : ::fcntl(state->descriptor, F_GETFL);
	}();
	const int mode = ::fcntl(descriptor, F_GETFL);
	if (parentMode < 0 || mode < 0 ||
		::fcntl(descriptor, F_SETFL, (mode & ~O_NONBLOCK) | (parentMode & O_NONBLOCK)) < 0)
		return fail(detail::socketError(errno));
#if defined(__APPLE__)
	const int noSignal = 1;
	if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal)) < 0)
		return fail(detail::socketError(errno));
#endif
	if (address) {
		const auto error = detail::addressFromNative(reinterpret_cast<sockaddr *>(&peer), address, addressLength);
		if (error)
			return fail(error);
	}
	return registerSocket(std::move(accepted));
}

int WINAPI getsockname(SOCKET handle, LPVOID address, int *length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("getsockname(0x%llx, %p, %p)\n", static_cast<unsigned long long>(handle), address, length);
	return querySocketAddress(handle, address, length, false);
}
int WINAPI getpeername(SOCKET handle, LPVOID address, int *length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("getpeername(0x%llx, %p, %p)\n", static_cast<unsigned long long>(handle), address, length);
	return querySocketAddress(handle, address, length, true);
}
} // namespace ws2
