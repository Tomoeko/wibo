#include "dbghelp.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "modules.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

constexpr DWORD kSymoptUndname = 0x2;
std::atomic<DWORD> g_symbolOptions{kSymoptUndname};

struct SymbolSession {
	std::vector<wibo::LoadedImageRange> images;
};

std::mutex g_symbolSessionsMutex;
std::unordered_map<HANDLE, SymbolSession> g_symbolSessions;

DWORD unsupportedSymbolQueryError(HANDLE handle) {
	std::lock_guard lock(g_symbolSessionsMutex);
	return g_symbolSessions.contains(handle) ? ERROR_NOT_SUPPORTED : ERROR_INVALID_HANDLE;
}

} // namespace

namespace dbghelp {

DWORD WINAPI SymGetOptions() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymGetOptions()\n");
	return g_symbolOptions.load(std::memory_order_relaxed);
}

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
	{
		std::lock_guard lock(g_symbolSessionsMutex);
		if (g_symbolSessions.contains(hProcess)) {
			return TRUE;
		}
	}
	if (fInvadeProcess && !kernel32::isPseudoCurrentProcessHandle(hProcess)) {
		auto process = wibo::handles().getAs<kernel32::ProcessObject>(hProcess);
		kernel32::setLastError(process ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	SymbolSession session;
	if (fInvadeProcess) {
		// Register mapped images; symbol and unwind data remain unavailable.
		session.images = wibo::loadedImageRanges();
	}
	std::lock_guard lock(g_symbolSessionsMutex);
	auto [entry, inserted] = g_symbolSessions.try_emplace(hProcess, std::move(session));
	if (inserted && fInvadeProcess) {
		kernel32::setLastError(ERROR_SUCCESS);
	}
	return TRUE;
}

BOOL WINAPI SymCleanup(HANDLE hProcess) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymCleanup(%p)\n", hProcess);
	std::lock_guard lock(g_symbolSessionsMutex);
	return g_symbolSessions.erase(hProcess) != 0;
}

PVOID WINAPI SymFunctionTableAccess64(HANDLE hProcess, DWORD64 AddrBase) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymFunctionTableAccess64(%p, 0x%llx)\n", hProcess, AddrBase);
	kernel32::setLastError(unsupportedSymbolQueryError(hProcess));
	return nullptr;
}

DWORD64 WINAPI SymGetModuleBase64(HANDLE hProcess, DWORD64 qwAddr) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymGetModuleBase64(%p, 0x%llx)\n", hProcess, qwAddr);
	std::lock_guard lock(g_symbolSessionsMutex);
	auto session = g_symbolSessions.find(hProcess);
	if (session == g_symbolSessions.end()) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return 0;
	}
	for (const wibo::LoadedImageRange &image : session->second.images) {
		if (qwAddr >= image.base && qwAddr - image.base < image.size) {
			return image.base;
		}
	}
	kernel32::setLastError(ERROR_MOD_NOT_FOUND);
	return 0;
}

BOOL WINAPI SymGetLineFromAddr64(HANDLE hProcess, DWORD64 qwAddr, PDWORD pdwDisplacement, PVOID Line64) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymGetLineFromAddr64(%p, 0x%llx, %p, %p)\n", hProcess, qwAddr, pdwDisplacement, Line64);
	kernel32::setLastError(unsupportedSymbolQueryError(hProcess));
	return FALSE;
}

BOOL WINAPI SymFromAddr(HANDLE hProcess, DWORD64 Address, PDWORD64 Displacement, PVOID Symbol) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymFromAddr(%p, 0x%llx, %p, %p)\n", hProcess, Address, Displacement, Symbol);
	kernel32::setLastError(unsupportedSymbolQueryError(hProcess));
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
