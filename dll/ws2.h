#pragma once

#include "types.h"

struct WSADATA {
	WORD wVersion;
	WORD wHighVersion;
#ifdef WIBO_GUEST_64
	WORD iMaxSockets;
	WORD iMaxUdpDg;
	GUEST_PTR lpVendorInfo;
	CHAR szDescription[257];
	CHAR szSystemStatus[129];
#else
	CHAR szDescription[257];
	CHAR szSystemStatus[129];
	WORD iMaxSockets;
	WORD iMaxUdpDg;
	GUEST_PTR lpVendorInfo;
#endif
};

struct ADDRINFOA {
	int ai_flags;
	int ai_family;
	int ai_socktype;
	int ai_protocol;
	SIZE_T ai_addrlen;
	GUEST_PTR ai_canonname;
	GUEST_PTR ai_addr;
	GUEST_PTR ai_next;
};

static_assert(sizeof(WSADATA) == (sizeof(GUEST_PTR) == 8 ? 408 : 400));
static_assert(sizeof(ADDRINFOA) == (sizeof(GUEST_PTR) == 8 ? 48 : 32));

using SOCKET = GUEST_PTR;

struct WSA_FD_SET {
	static constexpr DWORD kMaxCount = 65536;
	DWORD count;
	SOCKET sockets[1];
};
static_assert(offsetof(WSA_FD_SET, sockets) == sizeof(SOCKET));

namespace ws2 {

HANDLE WINAPI WSACreateEvent();
BOOL WINAPI WSACloseEvent(HANDLE event);
BOOL WINAPI WSASetEvent(HANDLE event);
BOOL WINAPI WSAResetEvent(HANDLE event);
DWORD WINAPI WSAWaitForMultipleEvents(DWORD count, const HANDLE *events, BOOL waitAll, DWORD timeout, BOOL alertable);
SOCKET WINAPI WSASocketA(int family, int type, int protocol, LPCVOID protocolInfo, UINT group, DWORD flags);
SOCKET WINAPI socket(int family, int type, int protocol);
int WINAPI closesocket(SOCKET handle);
int WINAPI bind(SOCKET handle, LPCVOID address, int length);
int WINAPI connect(SOCKET handle, LPCVOID address, int length);
int WINAPI listen(SOCKET handle, int backlog);
SOCKET WINAPI accept(SOCKET handle, LPVOID address, int *addressLength);
int WINAPI recv(SOCKET handle, LPSTR buffer, int length, int flags);
int WINAPI recvfrom(SOCKET handle, LPSTR buffer, int length, int flags, LPVOID address, int *addressLength);
LPSTR WINAPI inet_ntoa(ULONG address);
ULONG WINAPI inet_addr(LPCSTR text);
int WINAPI setsockopt(SOCKET handle, int level, int name, LPCSTR value, int length);
int WINAPI getsockopt(SOCKET handle, int level, int name, LPSTR value, int *length);
int WINAPI ioctlsocket(SOCKET handle, LONG command, ULONG *value);
int WINAPI getsockname(SOCKET handle, LPVOID address, int *length);
ULONG WINAPI(ntohl)(ULONG netlong);
ULONG WINAPI(htonl)(ULONG value);
USHORT WINAPI(ntohs)(USHORT value);
USHORT WINAPI(htons)(USHORT value);
int WINAPI WSAStartup(WORD wVersionRequired, WSADATA *lpWSAData);
int WINAPI WSACleanup();
int WINAPI WSAGetLastError();
void WINAPI WSASetLastError(int error);
int WINAPI getaddrinfo(LPCSTR node, LPCSTR service, const ADDRINFOA *hints, GUEST_PTR *result);
void WINAPI freeaddrinfo(ADDRINFOA *result);
int WINAPI gethostname(LPSTR name, int namelen);
GUEST_PTR WINAPI gethostbyname(LPCSTR name);
int WINAPI select(int nfds, LPVOID readfds, LPVOID writefds, LPVOID exceptfds, const void *timeout);
int WINAPI __WSAFDIsSet(SOCKET handle, const WSA_FD_SET *set);

} // namespace ws2
