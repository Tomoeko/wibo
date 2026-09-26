#pragma once

#include "types.h"
#include "minwinbase.h"

namespace kernel32 {

HANDLE WINAPI CreateIoCompletionPort(HANDLE file, HANDLE existing, ULONG_PTR key, DWORD concurrency);
BOOL WINAPI PostQueuedCompletionStatus(HANDLE handle, DWORD bytes, ULONG_PTR key, LPOVERLAPPED overlapped);
BOOL WINAPI GetQueuedCompletionStatus(HANDLE handle, LPDWORD bytes, ULONG_PTR *key, guest_ptr<OVERLAPPED> *overlapped,
									  DWORD milliseconds);
BOOL WINAPI GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred,
								  BOOL bWait);

} // namespace kernel32
