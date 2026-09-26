#pragma once

#include "kernel32/minwinbase.h"
#include "ws2.h"

namespace mswsock {
BOOL WINAPI AcceptEx(SOCKET listener, SOCKET accepted, LPVOID output, DWORD receiveLength, DWORD localLength,
					 DWORD remoteLength, LPDWORD received, LPOVERLAPPED overlapped);
void WINAPI GetAcceptExSockaddrs(LPVOID output, DWORD receiveLength, DWORD localLength, DWORD remoteLength,
								 GUEST_PTR *local, int *localSize, GUEST_PTR *remote, int *remoteSize);
} // namespace mswsock
