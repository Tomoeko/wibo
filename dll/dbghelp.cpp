#include "dbghelp.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "modules.h"

#include <atomic>

namespace {

std::atomic<DWORD> g_symbolOptions{0};

} // namespace

namespace dbghelp {

DWORD WINAPI SymSetOptions(DWORD SymOptions) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymSetOptions(0x%x)\n", SymOptions);
	g_symbolOptions.store(SymOptions, std::memory_order_relaxed);
	return SymOptions;
}

BOOL WINAPI SymInitialize(HANDLE hProcess, LPCSTR UserSearchPath, BOOL fInvadeProcess) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymInitialize(%p, %s, %d)\n", hProcess, UserSearchPath ? UserSearchPath : "<null>", fInvadeProcess);
	if (hProcess == NO_HANDLE) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return TRUE;
}

PVOID WINAPI SymFunctionTableAccess64(HANDLE hProcess, DWORD64 AddrBase) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymFunctionTableAccess64(%p, 0x%llx)\n", hProcess, AddrBase);
	return nullptr;
}

DWORD64 WINAPI SymGetModuleBase64(HANDLE hProcess, DWORD64 qwAddr) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymGetModuleBase64(%p, 0x%llx)\n", hProcess, qwAddr);
	wibo::ModuleInfo *module = wibo::moduleInfoFromAddress(reinterpret_cast<void *>(static_cast<uintptr_t>(qwAddr)));
	return module && module->executable
			   ? static_cast<DWORD64>(reinterpret_cast<uintptr_t>(module->executable->imageBase))
			   : 0;
}

BOOL WINAPI SymGetLineFromAddr64(HANDLE hProcess, DWORD64 qwAddr, PDWORD pdwDisplacement, PVOID Line64) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymGetLineFromAddr64(%p, 0x%llx, %p, %p)\n", hProcess, qwAddr, pdwDisplacement, Line64);
	if (pdwDisplacement) {
		*pdwDisplacement = 0;
	}
	return FALSE;
}

BOOL WINAPI SymFromAddr(HANDLE hProcess, DWORD64 Address, PDWORD64 Displacement, PVOID Symbol) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymFromAddr(%p, 0x%llx, %p, %p)\n", hProcess, Address, Displacement, Symbol);
	if (Displacement) {
		*Displacement = 0;
	}
	return FALSE;
}

BOOL WINAPI StackWalk64(DWORD MachineType, HANDLE hProcess, HANDLE hThread, PVOID StackFrame, PVOID ContextRecord,
						PVOID ReadMemoryRoutine, PVOID FunctionTableAccessRoutine, PVOID GetModuleBaseRoutine,
						PVOID TranslateAddress) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("StackWalk64(0x%x, %p, %p, %p, %p, %p, %p, %p, %p)\n", MachineType, hProcess, hThread, StackFrame,
			  ContextRecord, ReadMemoryRoutine, FunctionTableAccessRoutine, GetModuleBaseRoutine, TranslateAddress);
	return FALSE;
}

BOOL WINAPI MiniDumpWriteDump(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, DWORD DumpType, PVOID ExceptionParam,
							  PVOID UserStreamParam, PVOID CallbackParam) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("MiniDumpWriteDump(%p, %u, %p, 0x%x, %p, %p, %p)\n", hProcess, ProcessId, hFile, DumpType, ExceptionParam,
			  UserStreamParam, CallbackParam);
	kernel32::setLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

} // namespace dbghelp

#include "dbghelp_trampolines.h"

extern const wibo::ModuleStub lib_dbghelp = {
	(const char *[]){
		"dbghelp",
		nullptr,
	},
	dbghelpThunkByName,
	nullptr,
};
