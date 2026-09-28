#pragma once

#include "types.h"
#include "minwinbase.h"

namespace kernel32 {

struct FileObject;
NTSTATUS peekPipeControl(FileObject *pipe, void *output, ULONG length, ULONG_PTR &information);

BOOL WINAPI CreatePipe(PHANDLE hReadPipe, PHANDLE hWritePipe, LPSECURITY_ATTRIBUTES lpPipeAttributes, DWORD nSize);
BOOL WINAPI PeekNamedPipe(HANDLE hNamedPipe, LPVOID lpBuffer, DWORD nBufferSize, LPDWORD lpBytesRead,
						  LPDWORD lpTotalBytesAvail, LPDWORD lpBytesLeftThisMessage);
HANDLE WINAPI CreateNamedPipeA(LPCSTR lpName, DWORD dwOpenMode, DWORD dwPipeMode, DWORD nMaxInstances,
								 DWORD nOutBufferSize, DWORD nInBufferSize, DWORD nDefaultTimeOut,
								 LPSECURITY_ATTRIBUTES lpSecurityAttributes);
HANDLE WINAPI CreateNamedPipeW(LPCWSTR lpName, DWORD dwOpenMode, DWORD dwPipeMode, DWORD nMaxInstances,
							   DWORD nOutBufferSize, DWORD nInBufferSize, DWORD nDefaultTimeOut,
							   LPSECURITY_ATTRIBUTES lpSecurityAttributes);
BOOL WINAPI ConnectNamedPipe(HANDLE hNamedPipe, LPOVERLAPPED lpOverlapped);
BOOL WINAPI DisconnectNamedPipe(HANDLE hNamedPipe);
enum class NamedPipeCancelResult { NotPipe, NotFound, Cancelled };
NamedPipeCancelResult cancelNamedPipeConnect(HANDLE handle, LPOVERLAPPED overlapped, bool callerThreadOnly);
bool tryCreateFileNamedPipeA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
							 LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
							 DWORD dwFlagsAndAttributes, HANDLE &outHandle);

} // namespace kernel32
