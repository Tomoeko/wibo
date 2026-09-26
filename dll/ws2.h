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

namespace ws2 {

ULONG WINAPI(ntohl)(ULONG netlong);
int WINAPI WSAStartup(WORD wVersionRequired, WSADATA *lpWSAData);
int WINAPI WSACleanup();
int WINAPI WSAGetLastError();
int WINAPI getaddrinfo(LPCSTR node, LPCSTR service, const ADDRINFOA *hints, GUEST_PTR *result);
void WINAPI freeaddrinfo(ADDRINFOA *result);
int WINAPI gethostname(LPSTR name, int namelen);
GUEST_PTR WINAPI gethostbyname(LPCSTR name);
int WINAPI select(int nfds, LPVOID readfds, LPVOID writefds, LPVOID exceptfds, const void *timeout);

} // namespace ws2
