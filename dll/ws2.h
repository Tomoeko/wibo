#pragma once

#include "kernel32/minwinbase.h"
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

struct AFPROTOCOLS {
	int iAddressFamily;
	int iProtocol;
};
struct WSAQUERYSETW {
	DWORD dwSize;
	GUEST_PTR lpszServiceInstanceName;
	GUEST_PTR lpServiceClassId;
	GUEST_PTR lpVersion;
	GUEST_PTR lpszComment;
	DWORD dwNameSpace;
	GUEST_PTR lpNSProviderId;
	GUEST_PTR lpszContext;
	DWORD dwNumberOfProtocols;
	GUEST_PTR lpafpProtocols;
	GUEST_PTR lpszQueryString;
	DWORD dwNumberOfCsAddrs;
	GUEST_PTR lpcsaBuffer;
	DWORD dwOutputFlags;
	GUEST_PTR lpBlob;
};
static_assert(sizeof(WSAQUERYSETW) == (sizeof(GUEST_PTR) == 8 ? 120 : 60));

struct WSABUF {
	ULONG len;
	GUEST_PTR buf;
};
static_assert(sizeof(WSABUF) == (sizeof(GUEST_PTR) == 8 ? 16 : 8));

namespace ws2 {

HANDLE WINAPI WSACreateEvent();
BOOL WINAPI WSACloseEvent(HANDLE event);
BOOL WINAPI WSASetEvent(HANDLE event);
BOOL WINAPI WSAResetEvent(HANDLE event);
DWORD WINAPI WSAWaitForMultipleEvents(DWORD count, const HANDLE *events, BOOL waitAll, DWORD timeout, BOOL alertable);
SOCKET WINAPI WSASocketA(int family, int type, int protocol, LPCVOID protocolInfo, UINT group, DWORD flags);
SOCKET WINAPI socket(int family, int type, int protocol);
int WINAPI closesocket(SOCKET handle);
int WINAPI shutdown(SOCKET handle, int how);
int WINAPI bind(SOCKET handle, LPCVOID address, int length);
int WINAPI connect(SOCKET handle, LPCVOID address, int length);
int WINAPI listen(SOCKET handle, int backlog);
SOCKET WINAPI accept(SOCKET handle, LPVOID address, int *addressLength);
int WINAPI recv(SOCKET handle, LPSTR buffer, int length, int flags);
int WINAPI recvfrom(SOCKET handle, LPSTR buffer, int length, int flags, LPVOID address, int *addressLength);
LPSTR WINAPI inet_ntoa(ULONG address);
ULONG WINAPI inet_addr(LPCSTR text);
int WINAPI WSAStringToAddressW(LPWSTR input, int family, LPCVOID protocol, LPVOID output, int *length);
int WINAPI setsockopt(SOCKET handle, int level, int name, LPCSTR value, int length);
int WINAPI getsockopt(SOCKET handle, int level, int name, LPSTR value, int *length);
int WINAPI WSARecv(SOCKET handle, const WSABUF *buffers, DWORD count, LPDWORD received, LPDWORD flags,
				   LPOVERLAPPED overlapped, GUEST_PTR completionRoutine);
int WINAPI WSASend(SOCKET handle, const WSABUF *buffers, DWORD count, LPDWORD sent, DWORD flags,
				   LPOVERLAPPED overlapped, GUEST_PTR completionRoutine);
BOOL WINAPI WSAGetOverlappedResult(SOCKET handle, LPOVERLAPPED overlapped, LPDWORD transferred, BOOL wait,
								   LPDWORD flags);
int WINAPI WSAIoctl(SOCKET handle, DWORD operation, LPCVOID input, DWORD inputLength, LPVOID output, DWORD outputLength,
					LPDWORD returned, LPOVERLAPPED overlapped, GUEST_PTR completionRoutine);
int WINAPI send(SOCKET handle, LPCSTR buffer, int length, int flags);
int WINAPI ioctlsocket(SOCKET handle, LONG command, ULONG *value);
int WINAPI getsockname(SOCKET handle, LPVOID address, int *length);
int WINAPI getpeername(SOCKET handle, LPVOID address, int *length);
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
int WINAPI WSALookupServiceBeginW(const WSAQUERYSETW *restrictions, DWORD flags, LPHANDLE lookup);
int WINAPI WSALookupServiceNextW(HANDLE lookup, DWORD flags, LPDWORD length, WSAQUERYSETW *result);
int WINAPI WSALookupServiceEnd(HANDLE lookup);
int WINAPI gethostname(LPSTR name, int namelen);
GUEST_PTR WINAPI gethostbyname(LPCSTR name);
int WINAPI select(int nfds, LPVOID readfds, LPVOID writefds, LPVOID exceptfds, const void *timeout);
int WINAPI __WSAFDIsSet(SOCKET handle, const WSA_FD_SET *set);

} // namespace ws2
