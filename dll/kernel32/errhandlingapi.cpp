#include "errhandlingapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"

#include <atomic>

namespace {

constexpr DWORD kMsVcThreadNameException = 0x406D1388;
constexpr DWORD kExceptionNoncontinuable = 0x1;

LPTOP_LEVEL_EXCEPTION_FILTER g_topLevelExceptionFilter = nullptr;
std::atomic<UINT> g_processErrorMode{0};
thread_local DWORD g_threadErrorMode = 0;

} // namespace

namespace kernel32 {

DWORD getLastError() { return currentThreadTeb->LastErrorValue; }

void setLastError(DWORD error) { currentThreadTeb->LastErrorValue = error; }

void setLastErrorFromErrno() { setLastError(wibo::winErrorFromErrno(errno)); }

DWORD WINAPI GetLastError() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLastError() -> %u\n", getLastError());
	return currentThreadTeb->LastErrorValue;
}

void WINAPI SetLastError(DWORD dwErrCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetLastError(%u)\n", dwErrCode);
	currentThreadTeb->LastErrorValue = dwErrCode;
}

void WINAPI RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags, DWORD nNumberOfArguments,
						   const ULONG_PTR *lpArguments) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RaiseException(0x%x, 0x%x, %u, %p)\n", dwExceptionCode, dwExceptionFlags, nNumberOfArguments,
			  lpArguments);
	// Visual C++ uses this continuable exception as an out-of-band debugger
	// notification for naming a thread. It is intentionally raised inside an
	// SEH guard and is not a process-termination request. Wibo does not yet
	// dispatch guest SEH frames, so consume the notification and let execution
	// continue exactly as the guarded Windows call does.
	if (dwExceptionCode == kMsVcThreadNameException && (dwExceptionFlags & kExceptionNoncontinuable) == 0) {
		return;
	}
	(void)dwExceptionFlags;
	(void)nNumberOfArguments;
	(void)lpArguments;
	exitInternal(dwExceptionCode);
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI
SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetUnhandledExceptionFilter(%p)\n", lpTopLevelExceptionFilter);
	LPTOP_LEVEL_EXCEPTION_FILTER previous = g_topLevelExceptionFilter;
	g_topLevelExceptionFilter = lpTopLevelExceptionFilter;
	return previous;
}

LONG WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS ExceptionInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: UnhandledExceptionFilter(%p)\n", ExceptionInfo);
	return EXCEPTION_EXECUTE_HANDLER;
}

UINT WINAPI SetErrorMode(UINT uMode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetErrorMode(%u)\n", uMode);
	return g_processErrorMode.exchange(uMode, std::memory_order_relaxed);
}

UINT WINAPI GetErrorMode() {
	HOST_CONTEXT_GUARD();
	const UINT mode = g_processErrorMode.load(std::memory_order_relaxed);
	DEBUG_LOG("GetErrorMode() -> %u\n", mode);
	return mode;
}

DWORD WINAPI GetThreadErrorMode() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetThreadErrorMode() -> %u\n", g_threadErrorMode);
	return g_threadErrorMode;
}

BOOL WINAPI SetThreadErrorMode(DWORD dwNewMode, LPDWORD lpOldMode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetThreadErrorMode(%u, %p)\n", dwNewMode, lpOldMode);
	constexpr DWORD validModes = 0x8003;
	if ((dwNewMode & ~validModes) != 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (lpOldMode) {
		*lpOldMode = g_threadErrorMode;
	}
	g_threadErrorMode = dwNewMode;
	return TRUE;
}

HRESULT WINAPI WerSetFlags(DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	// Wibo does not provide a host-side Windows Error Reporting service. The
	// flags only control how WER handles a later failure, so acknowledging them
	// is sufficient and must not alter guest execution or host crash handling.
	DEBUG_LOG("WerSetFlags(0x%x)\n", dwFlags);
	return S_OK;
}

HRESULT WINAPI WerRegisterRuntimeExceptionModule(LPCWSTR callbackDll, PVOID context) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WerRegisterRuntimeExceptionModule(%p, %p): reporting service unavailable\n", callbackDll, context);
	// Registration requires a service that can load guest callbacks after a process failure.
	return static_cast<HRESULT>(0x80070000U | ERROR_NOT_SUPPORTED);
}

} // namespace kernel32
