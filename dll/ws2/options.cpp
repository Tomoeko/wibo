#include "ws2/internal.h"

#include "common.h"
#include "context.h"
#include "modules.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/time.h>

extern const wibo::ModuleStub lib_mswsock;

namespace {
enum class ValueKind { Integer, Boolean, Linger, Timeout, Error, Listening };
struct Option {
	int level = 0, name = 0;
	ValueKind kind = ValueKind::Integer;
	bool writable = true;
};
bool mapOption(int level, int name, Option &result) {
	if (level == 0xFFFF) {
		result.level = SOL_SOCKET;
		switch (name) {
		case 2:
			result = {SOL_SOCKET, SO_ACCEPTCONN, ValueKind::Listening, false};
			break;
		case 4:
			result = {SOL_SOCKET, SO_REUSEADDR, ValueKind::Boolean};
			break;
		case 8:
			result = {SOL_SOCKET, SO_KEEPALIVE, ValueKind::Boolean};
			break;
		case 0x10:
			result = {SOL_SOCKET, SO_DONTROUTE, ValueKind::Boolean};
			break;
		case 0x20:
			result = {SOL_SOCKET, SO_BROADCAST, ValueKind::Boolean};
			break;
		case 0x80:
			result = {SOL_SOCKET, SO_LINGER, ValueKind::Linger};
			break;
		case 0x100:
			result = {SOL_SOCKET, SO_OOBINLINE, ValueKind::Boolean};
			break;
		case 0x1001:
			result.name = SO_SNDBUF;
			break;
		case 0x1002:
			result.name = SO_RCVBUF;
			break;
		case 0x1005:
			result = {SOL_SOCKET, SO_SNDTIMEO, ValueKind::Timeout};
			break;
		case 0x1006:
			result = {SOL_SOCKET, SO_RCVTIMEO, ValueKind::Timeout};
			break;
		case 0x1007:
			result = {SOL_SOCKET, SO_ERROR, ValueKind::Error, false};
			break;
		case 0x1008:
			result = {SOL_SOCKET, SO_TYPE, ValueKind::Integer, false};
			break;
		default:
			return false;
		}
		return true;
	}
	if (level == 6 && name == 1) {
		result = {IPPROTO_TCP, TCP_NODELAY, ValueKind::Boolean};
		return true;
	}
	if (level == 41 && name == 27) {
		result = {IPPROTO_IPV6, IPV6_V6ONLY, ValueKind::Boolean};
		return true;
	}
	return false;
}
struct GuestLinger {
	uint16_t enabled, seconds;
};
static_assert(sizeof(GuestLinger) == 4);

} // namespace

namespace ws2 {
int WINAPI setsockopt(SOCKET handle, int level, int name, LPCSTR value, int length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("setsockopt(0x%llx, %d, %d, %p, %d)\n", static_cast<unsigned long long>(handle), level, name, value,
			  length);
	const auto state = detail::findSocket(handle);
	if (!state)
		return -1;
	if (level == 0xFFFF && name == 0x700B) {
		if (!value || length != sizeof(SOCKET))
			return detail::failSocket(10014);
		SOCKET listenerHandle;
		std::memcpy(&listenerHandle, value, sizeof(listenerHandle));
		const auto listener = detail::findSocket(listenerHandle);
		if (!listener || !listener->listening || listener->family != state->family)
			return detail::failSocket(10022);
		std::lock_guard lock(state->ioMutex);
		if (!state->acceptedByExtension)
			return detail::failSocket(10022);
		if (!detail::copySocketOptions(listener->descriptor, state->descriptor))
			return detail::failSocket(detail::socketError(errno));
		return 0;
	}
	Option option;
	if (!mapOption(level, name, option) || !option.writable)
		return detail::failSocket(10042);
	if (!value || length < 4)
		return detail::failSocket(10014);
	int status;
	if (option.kind == ValueKind::Linger) {
		GuestLinger input{};
		std::memcpy(&input, value, sizeof(input));
		const linger native{input.enabled != 0, input.seconds};
		status = ::setsockopt(state->descriptor, option.level, option.name, &native, sizeof(native));
	} else if (option.kind == ValueKind::Timeout) {
		uint32_t milliseconds = 0;
		std::memcpy(&milliseconds, value, sizeof(milliseconds));
		const timeval native{static_cast<time_t>(milliseconds / 1000),
							 static_cast<suseconds_t>((milliseconds % 1000) * 1000)};
		status = ::setsockopt(state->descriptor, option.level, option.name, &native, sizeof(native));
	} else {
		int native = 0;
		std::memcpy(&native, value, sizeof(native));
		if (option.kind == ValueKind::Boolean)
			native = native != 0;
		status = ::setsockopt(state->descriptor, option.level, option.name, &native, sizeof(native));
	}
	return status < 0 ? detail::failSocket(detail::socketError(errno)) : 0;
}
int WINAPI getsockopt(SOCKET handle, int level, int name, LPSTR value, int *length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("getsockopt(0x%llx, %d, %d, %p, %p)\n", static_cast<unsigned long long>(handle), level, name, value,
			  length);
	const auto state = detail::findSocket(handle);
	if (!state)
		return -1;
	Option option;
	if (!mapOption(level, name, option))
		return detail::failSocket(10042);
	if (!value || !length || *length < 4)
		return detail::failSocket(10014);
	if (option.kind == ValueKind::Listening) {
		const int result = state->listening.load() ? 1 : 0;
		std::memcpy(value, &result, sizeof(result));
	} else if (option.kind == ValueKind::Linger) {
		linger native{};
		socklen_t nativeLength = sizeof(native);
		if (::getsockopt(state->descriptor, option.level, option.name, &native, &nativeLength) < 0)
			return detail::failSocket(detail::socketError(errno));
		const GuestLinger result{static_cast<uint16_t>(native.l_onoff != 0), static_cast<uint16_t>(native.l_linger)};
		std::memcpy(value, &result, sizeof(result));
	} else if (option.kind == ValueKind::Timeout) {
		timeval native{};
		socklen_t nativeLength = sizeof(native);
		if (::getsockopt(state->descriptor, option.level, option.name, &native, &nativeLength) < 0)
			return detail::failSocket(detail::socketError(errno));
		const auto milliseconds =
			static_cast<uint64_t>(native.tv_sec) * 1000 + static_cast<uint64_t>(native.tv_usec) / 1000;
		const auto result =
			static_cast<uint32_t>(std::min<uint64_t>(milliseconds, std::numeric_limits<uint32_t>::max()));
		std::memcpy(value, &result, sizeof(result));
	} else {
		int native = 0;
		socklen_t nativeLength = sizeof(native);
		if (::getsockopt(state->descriptor, option.level, option.name, &native, &nativeLength) < 0)
			return detail::failSocket(detail::socketError(errno));
		if (option.kind == ValueKind::Boolean)
			native = native != 0;
		else if (option.kind == ValueKind::Error) {
			if (native)
				state->connecting.store(false);
			else if (state->connecting.load()) {
				sockaddr_storage peer{};
				socklen_t size = sizeof(peer);
				if (::getpeername(state->descriptor, reinterpret_cast<sockaddr *>(&peer), &size) == 0)
					state->connecting.store(false);
			}
			native = native ? detail::socketError(native) : 0;
			detail::setLastError(0);
		}
		std::memcpy(value, &native, sizeof(native));
	}
	*length = 4;
	return 0;
}
int WINAPI ioctlsocket(SOCKET handle, LONG command, ULONG *value) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ioctlsocket(0x%llx, 0x%x, %p)\n", static_cast<unsigned long long>(handle), static_cast<DWORD>(command),
			  value);
	const auto state = detail::findSocket(handle);
	if (!state)
		return -1;
	if (!value)
		return detail::failSocket(10014);
	const uint32_t operation = static_cast<uint32_t>(command);
	if (operation != 0x8004667EU && operation != 0x4004667FU)
		return detail::failSocket(10022);
	std::lock_guard lock(state->ioMutex);
	if (operation == 0x8004667EU && state->pendingAccepts) {
		state->acceptOriginalFlags =
			*value ? state->acceptOriginalFlags | O_NONBLOCK : state->acceptOriginalFlags & ~O_NONBLOCK;
		return 0;
	}
	int native = *value != 0;
	if (::ioctl(state->descriptor, operation == 0x8004667EU ? FIONBIO : FIONREAD, &native) < 0)
		return detail::failSocket(detail::socketError(errno));
	if (operation == 0x4004667FU)
		*value = static_cast<ULONG>(native);
	return 0;
}
int WINAPI WSAIoctl(SOCKET handle, DWORD operation, LPCVOID input, DWORD inputLength, LPVOID output, DWORD outputLength,
					LPDWORD returned, LPOVERLAPPED overlapped, GUEST_PTR completionRoutine) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSAIoctl(0x%llx, 0x%x, %p, %u, %p, %u, %p, %p, %p)\n", static_cast<unsigned long long>(handle),
			  operation, input, inputLength, output, outputLength, returned, overlapped,
			  fromGuestPtr<void>(completionRoutine));
	if (!detail::findSocket(handle))
		return -1;
	if (!returned)
		return detail::failSocket(10014);
	if (overlapped || completionRoutine)
		return detail::failSocket(10045);
	if (operation == 0xC8000006U) {
		if (!input || inputLength < sizeof(GUID) || !output || outputLength < sizeof(GUEST_PTR))
			return detail::failSocket(10014);
		GUID id{};
		std::memcpy(&id, input, sizeof(id));
		constexpr GUID acceptId{0xb5367df1, 0xcbac, 0x11cf, {0x95, 0xca, 0, 0x80, 0x5f, 0x48, 0xa1, 0x92}};
		constexpr GUID addressId{0xb5367df2, 0xcbac, 0x11cf, {0x95, 0xca, 0, 0x80, 0x5f, 0x48, 0xa1, 0x92}};
		const char *name = std::memcmp(&id, &acceptId, sizeof(id)) == 0	   ? "AcceptEx"
						   : std::memcmp(&id, &addressId, sizeof(id)) == 0 ? "GetAcceptExSockaddrs"
																		   : nullptr;
		if (!name)
			return detail::failSocket(10022);
		const GUEST_PTR pointer = toGuestPtr(lib_mswsock.byName(name));
		if (!pointer)
			return detail::failSocket(10045);
		std::memcpy(output, &pointer, sizeof(pointer));
		*returned = sizeof(pointer);
		return 0;
	}
	if (operation == 0x8004667EU) {
		if (!input || inputLength < sizeof(ULONG))
			return detail::failSocket(10014);
		ULONG value;
		std::memcpy(&value, input, sizeof(value));
		const int result = ioctlsocket(handle, static_cast<LONG>(operation), &value);
		if (!result)
			*returned = 0;
		return result;
	}
	if (operation == 0x4004667FU) {
		if (!output || outputLength < sizeof(ULONG))
			return detail::failSocket(10014);
		ULONG value = 0;
		const int result = ioctlsocket(handle, static_cast<LONG>(operation), &value);
		if (!result) {
			std::memcpy(output, &value, sizeof(value));
			*returned = sizeof(value);
		}
		return result;
	}
	return detail::failSocket(10045);
}
} // namespace ws2
