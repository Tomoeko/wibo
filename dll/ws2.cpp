#include "ws2.h"

#include "common.h"
#include "context.h"
#include "modules.h"
#include "ws2/internal.h"

#include <algorithm>
#include <atomic>

#include <cstring>
#include <unistd.h>

#ifdef ntohl
#undef ntohl
#endif
#ifdef htonl
#undef htonl
#endif
#ifdef ntohs
#undef ntohs
#endif
#ifdef htons
#undef htons
#endif

namespace {

constexpr int SOCKET_ERROR = -1;
constexpr int WSAEFAULT = 10014;
constexpr int WSAHOST_NOT_FOUND = 11001;
constexpr int WSANOTINITIALISED = 10093;

thread_local int g_lastError = 0;
std::atomic<unsigned> g_startupCount = 0;

WORD makeVersion(BYTE major, BYTE minor) { return static_cast<WORD>(major | (minor << 8)); }

} // namespace

namespace ws2::detail {

void setLastError(int error) { g_lastError = error; }

bool requireStarted() {
	if (g_startupCount > 0) {
		return true;
	}
	setLastError(WSANOTINITIALISED);
	return false;
}

} // namespace ws2::detail

namespace ws2 {

using detail::requireStarted;
using detail::setLastError;

ULONG WINAPI ntohl(ULONG netlong) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ntohl(0x%x)\n", netlong);
	return ((netlong & 0xFF) << 24) | ((netlong & 0xFF00) << 8) | ((netlong & 0xFF0000) >> 8) |
		   ((netlong & 0xFF000000) >> 24);
}

ULONG WINAPI htonl(ULONG value) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("htonl(0x%x)\n", value);
	return __builtin_bswap32(value);
}
USHORT WINAPI ntohs(USHORT value) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ntohs(0x%x)\n", value);
	return __builtin_bswap16(value);
}
USHORT WINAPI htons(USHORT value) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("htons(0x%x)\n", value);
	return __builtin_bswap16(value);
}

int WINAPI WSAStartup(WORD wVersionRequired, WSADATA *lpWSAData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSAStartup(0x%x, %p)\n", wVersionRequired, lpWSAData);
	if (!lpWSAData) {
		setLastError(WSAEFAULT);
		return WSAEFAULT;
	}

	std::memset(lpWSAData, 0, sizeof(*lpWSAData));
	const BYTE major = wVersionRequired & 0xFF;
	const BYTE minor = wVersionRequired >> 8;
	if (!major)
		return 10092;
	lpWSAData->wVersion = major == 1 ? makeVersion(1, std::min<BYTE>(minor, 1))
									 : makeVersion(2, major == 2 ? std::min<BYTE>(minor, 2) : 2);
	lpWSAData->wHighVersion = makeVersion(2, 2);
	std::strncpy(lpWSAData->szDescription, "Host socket services", sizeof(lpWSAData->szDescription) - 1);
	std::strncpy(lpWSAData->szSystemStatus, "Running", sizeof(lpWSAData->szSystemStatus) - 1);
	lpWSAData->iMaxSockets = 0x7fff;
	lpWSAData->iMaxUdpDg = 65467; // 65535 - max IPv4 header (60) - UDP header (8)

	++g_startupCount;
	setLastError(0);
	return 0;
}

int WINAPI WSACleanup() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSACleanup()\n");
	unsigned count = g_startupCount.load();
	while (count && !g_startupCount.compare_exchange_weak(count, count - 1)) {
	}
	if (!count) {
		setLastError(WSANOTINITIALISED);
		return SOCKET_ERROR;
	}

	if (count == 1)
		detail::cleanupSockets();
	setLastError(0);
	return 0;
}

int WINAPI WSAGetLastError() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSAGetLastError() -> %d\n", g_lastError);
	return g_lastError;
}

int WINAPI gethostname(LPSTR name, int namelen) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("gethostname(%p, %d)\n", name, namelen);
	if (!requireStarted()) {
		return SOCKET_ERROR;
	}
	if (!name || namelen <= 0) {
		setLastError(WSAEFAULT);
		return SOCKET_ERROR;
	}

	char host[256] = {};
	if (::gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0') {
		std::strncpy(host, "localhost", sizeof(host) - 1);
	}

	size_t length = std::strlen(host);
	if (static_cast<size_t>(namelen) <= length) {
		setLastError(WSAEFAULT);
		return SOCKET_ERROR;
	}

	std::memcpy(name, host, length + 1);
	setLastError(0);
	return 0;
}

GUEST_PTR WINAPI gethostbyname(LPCSTR name) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("gethostbyname(%s)\n", name ? name : "(null)");
	if (!requireStarted()) {
		return GUEST_NULL;
	}
	setLastError(WSAHOST_NOT_FOUND);
	return GUEST_NULL;
}

int WINAPI select(int nfds, LPVOID readfds, LPVOID writefds, LPVOID exceptfds, const void *timeout) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("select(%d, %p, %p, %p, %p)\n", nfds, readfds, writefds, exceptfds, timeout);
	if (!requireStarted()) {
		return SOCKET_ERROR;
	}
	(void)nfds;
	(void)readfds;
	(void)writefds;
	(void)exceptfds;
	(void)timeout;
	setLastError(0);
	return 0;
}

} // namespace ws2

#include "ws2_trampolines.h"

static const char *resolveNameByOrdinal(uint16_t ordinal) {
	// GHS 5.3.22 imports WS2_32.dll with the legacy winsock ordinal table.
	// Keep these mappings tied to observed call sites rather than modern WS2_32 export ordinals.
	switch (ordinal) {
	case 2:
		return "bind";
	case 3:
		return "closesocket";
	case 6:
		return "getsockname";
	case 23:
		return "socket";
	case 7:
		return "getsockopt";
	case 10:
		return "ioctlsocket";
	case 21:
		return "setsockopt";
	case 8:
		return "htonl";
	case 9:
		return "htons";
	case 15:
		return "ntohs";
	case 14:
		return "ntohl";
	case 18:
		return "select";
	case 52:
		return "gethostbyname";
	case 57:
		return "gethostname";
	case 115:
		return "WSAStartup";
	case 116:
		return "WSACleanup";
	default:
		return nullptr;
	}
}

extern const wibo::ModuleStub lib_ws2 = {
	(const char *[]){
		"WS2_32",
		nullptr,
	},
	ws2ThunkByName,
	resolveNameByOrdinal,
};
