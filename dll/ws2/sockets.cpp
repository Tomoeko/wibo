#include "ws2/internal.h"

#include "common.h"
#include "context.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <unistd.h>
#include <unordered_map>

namespace {
constexpr SOCKET kInvalidSocket = static_cast<SOCKET>(-1);
constexpr int kWinInet6 = 23;
struct SocketRegistry {
	std::mutex mutex;
	std::unordered_map<SOCKET, std::shared_ptr<ws2::detail::Socket>> sockets;
	SOCKET next = 0x40000000;
};
SocketRegistry &socketRegistry() {
	static SocketRegistry registry;
	return registry;
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
	auto &registry = socketRegistry();
	std::lock_guard lock(registry.mutex);
	if (!requireStarted())
		return kInvalidSocket;
	if (registry.next == kInvalidSocket)
		return fail(10024);
	const SOCKET handle = registry.next++;
	registry.sockets.emplace(handle, std::move(state));
	return handle;
}
} // namespace

namespace ws2::detail {
Socket::~Socket() { ::close(descriptor); }
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
void cleanupSockets() {
	auto &registry = socketRegistry();
	std::lock_guard lock(registry.mutex);
	for (const auto &[handle, state] : registry.sockets)
		::shutdown(state->descriptor, SHUT_RDWR);
	registry.sockets.clear();
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
	::shutdown(found->second->descriptor, SHUT_RDWR);
	registry.sockets.erase(found);
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
int WINAPI getsockname(SOCKET handle, LPVOID address, int *length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("getsockname(0x%llx, %p, %p)\n", static_cast<unsigned long long>(handle), address, length);
	const auto state = detail::findSocket(handle);
	if (!state)
		return -1;
	sockaddr_storage native{};
	socklen_t nativeLength = sizeof(native);
	if (::getsockname(state->descriptor, reinterpret_cast<sockaddr *>(&native), &nativeLength) < 0)
		return detail::failSocket(detail::socketError(errno));
	const auto *ip = reinterpret_cast<const sockaddr_in *>(&native);
	if (ip->sin_port == 0)
		return detail::failSocket(10022);
	const int status = detail::addressFromNative(reinterpret_cast<sockaddr *>(&native), address, length);
	return status ? detail::failSocket(status) : 0;
}
} // namespace ws2
