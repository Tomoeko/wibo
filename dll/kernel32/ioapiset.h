#pragma once

#include "types.h"
#include "minwinbase.h"

namespace kernel32 {
BOOL WINAPI CancelIo(HANDLE handle);
BOOL WINAPI CancelIoEx(HANDLE handle, LPOVERLAPPED overlapped);
BOOL WINAPI DeviceIoControl(HANDLE device, DWORD controlCode, LPVOID input, DWORD inputSize, LPVOID output,
							DWORD outputSize, LPDWORD bytesReturned, LPOVERLAPPED overlapped);

HANDLE WINAPI CreateIoCompletionPort(HANDLE file, HANDLE existing, ULONG_PTR key, DWORD concurrency);
BOOL WINAPI PostQueuedCompletionStatus(HANDLE handle, DWORD bytes, ULONG_PTR key, LPOVERLAPPED overlapped);
BOOL WINAPI GetQueuedCompletionStatus(HANDLE handle, LPDWORD bytes, ULONG_PTR *key, guest_ptr<OVERLAPPED> *overlapped,
									  DWORD milliseconds);
BOOL WINAPI GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred,
								  BOOL bWait);

} // namespace kernel32
