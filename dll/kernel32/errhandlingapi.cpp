#include "errhandlingapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "vectored_exception.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

namespace {

constexpr DWORD kMsVcThreadNameException = 0x406D1388;
constexpr DWORD kExceptionNoncontinuable = 0x1;

LPTOP_LEVEL_EXCEPTION_FILTER g_topLevelExceptionFilter = nullptr;
std::atomic<UINT> g_processErrorMode{0};
thread_local DWORD g_threadErrorMode = 0;

struct VectoredExceptionRegistration {
	PVECTORED_EXCEPTION_HANDLER handler;
	ULONG_PTR token;
	size_t references;
	bool registered;
	VectoredExceptionRegistration *previous;
	VectoredExceptionRegistration *next;
};

std::mutex g_vectoredExceptionMutex;
VectoredExceptionRegistration *g_vectoredExceptionFirst = nullptr;
VectoredExceptionRegistration *g_vectoredExceptionLast = nullptr;
ULONG_PTR g_nextVectoredExceptionToken = 1;

// All list links and reference counts are protected by the registry mutex.
void releaseVectoredExceptionRegistration(VectoredExceptionRegistration *entry) {
	if (--entry->references != 0) {
		return;
	}
	if (entry->previous) {
		entry->previous->next = entry->next;
	} else {
		g_vectoredExceptionFirst = entry->next;
	}
	if (entry->next) {
		entry->next->previous = entry->previous;
	} else {
		g_vectoredExceptionLast = entry->previous;
	}
	std::free(entry);
}

} // namespace

namespace wibo {

LONG invokeVectoredExceptionHandlers(PEXCEPTION_POINTERS exceptionInfo, VectoredExceptionInvoker invoke) {
	std::unique_lock lock(g_vectoredExceptionMutex);
	auto *entry = g_vectoredExceptionFirst;
	while (entry) {
		++entry->references;
		const auto handler = entry->handler;
		lock.unlock();
		const LONG result = invoke(handler, exceptionInfo);
		lock.lock();
		// Retain the active entry until after advancing through the live list.
		// A self-removal is deferred, so nested invocations can still reach it.
		auto *next = entry->next;
		releaseVectoredExceptionRegistration(entry);
		if (result == EXCEPTION_CONTINUE_EXECUTION) {
			return result;
		}
		entry = next;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace wibo

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

PVOID WINAPI AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("AddVectoredExceptionHandler(%u, %p)\n", First, Handler);
	auto *entry = static_cast<VectoredExceptionRegistration *>(std::malloc(sizeof(VectoredExceptionRegistration)));
	if (!entry) {
		return nullptr;
	}
	std::unique_lock lock(g_vectoredExceptionMutex);
	if (g_nextVectoredExceptionToken == 0) {
		std::free(entry);
		return nullptr;
	}
	const ULONG_PTR token = g_nextVectoredExceptionToken++;
	*entry = {Handler, token, 1, true, nullptr, nullptr};
	if (First) {
		entry->next = g_vectoredExceptionFirst;
		if (entry->next) {
			entry->next->previous = entry;
		} else {
			g_vectoredExceptionLast = entry;
		}
		g_vectoredExceptionFirst = entry;
	} else {
		entry->previous = g_vectoredExceptionLast;
		if (entry->previous) {
			entry->previous->next = entry;
		} else {
			g_vectoredExceptionFirst = entry;
		}
		g_vectoredExceptionLast = entry;
	}
	// Tokens are never reused, so a stale handle cannot remove a later registration.
	return reinterpret_cast<PVOID>(static_cast<uintptr_t>(token));
}

ULONG WINAPI RemoveVectoredExceptionHandler(PVOID Handle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RemoveVectoredExceptionHandler(%p)\n", Handle);
	const auto token = reinterpret_cast<uintptr_t>(Handle);
	std::unique_lock lock(g_vectoredExceptionMutex);
	for (auto *entry = g_vectoredExceptionFirst; entry; entry = entry->next) {
		if (entry->token == token && entry->registered) {
			entry->registered = false;
			releaseVectoredExceptionRegistration(entry);
			return 1;
		}
	}
	return 0;
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
