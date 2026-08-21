#pragma once

#include "types.h"

using DWORD64 = ULONGLONG;
using PDWORD64 = DWORD64 *;

namespace dbghelp {

DWORD WINAPI SymSetOptions(DWORD SymOptions);
BOOL WINAPI SymInitialize(HANDLE hProcess, LPCSTR UserSearchPath, BOOL fInvadeProcess);
PVOID WINAPI SymFunctionTableAccess64(HANDLE hProcess, DWORD64 AddrBase);
DWORD64 WINAPI SymGetModuleBase64(HANDLE hProcess, DWORD64 qwAddr);
BOOL WINAPI SymGetLineFromAddr64(HANDLE hProcess, DWORD64 qwAddr, PDWORD pdwDisplacement, PVOID Line64);
BOOL WINAPI SymFromAddr(HANDLE hProcess, DWORD64 Address, PDWORD64 Displacement, PVOID Symbol);
BOOL WINAPI StackWalk64(DWORD MachineType, HANDLE hProcess, HANDLE hThread, PVOID StackFrame, PVOID ContextRecord,
						PVOID ReadMemoryRoutine, PVOID FunctionTableAccessRoutine, PVOID GetModuleBaseRoutine,
						PVOID TranslateAddress);
BOOL WINAPI MiniDumpWriteDump(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, DWORD DumpType, PVOID ExceptionParam,
							  PVOID UserStreamParam, PVOID CallbackParam);

} // namespace dbghelp
