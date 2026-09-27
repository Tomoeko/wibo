#include "wincon.h"

#include "context.h"
#include "errors.h"
#include "files.h"
#include "handles.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace kernel32 {

namespace {
struct ConsoleControlHandler {
	PHANDLER_ROUTINE routine;
	std::unique_ptr<ConsoleControlHandler> next;
};

struct ConsoleControlHandlers {
	std::mutex mutex;
	std::unique_ptr<ConsoleControlHandler> first;
	~ConsoleControlHandlers() {
		while (first) {
			auto removed = std::move(first);
			first = std::move(removed->next);
		}
	}
};

ConsoleControlHandlers g_consoleControlHandlers;
std::atomic_bool g_consoleControlCIgnore{false};

BOOL rejectUnavailableConsole(HANDLE handle) {
	auto file = wibo::handles().getAs<FileObject>(handle);
	if (!file || !file->valid() || !isatty(file->fd)) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	// A host terminal is not a Windows console buffer with a mode/event backend.
	setLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
}
} // namespace

BOOL WINAPI AttachConsole(DWORD processId) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("AttachConsole(%u)\n", processId);
	const auto target = processId == static_cast<DWORD>(-1) ? static_cast<DWORD>(getppid()) : processId;
	if (!target || target > static_cast<DWORD>(std::numeric_limits<pid_t>::max())) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (kill(static_cast<pid_t>(target), 0) != 0 && errno != EPERM) {
		setLastError(errno == ESRCH ? ERROR_INVALID_PARAMETER : wibo::winErrorFromErrno(errno));
		return FALSE;
	}
	// Neither this process nor its parent has a registered console session.
	// Cross-process console attachment requires a session backend.
	setLastError(target == static_cast<DWORD>(getpid()) || target == static_cast<DWORD>(getppid())
					 ? ERROR_INVALID_HANDLE
					 : ERROR_NOT_SUPPORTED);
	return FALSE;
}

BOOL WINAPI GetConsoleMode(HANDLE hConsoleHandle, LPDWORD lpMode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetConsoleMode(%p, %p)\n", hConsoleHandle, lpMode);
	return rejectUnavailableConsole(hConsoleHandle);
}

BOOL WINAPI SetConsoleMode(HANDLE hConsoleHandle, DWORD dwMode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetConsoleMode(%p, 0x%x)\n", hConsoleHandle, dwMode);
	return rejectUnavailableConsole(hConsoleHandle);
}

UINT WINAPI GetConsoleCP() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetConsoleCP()\n");
	// Standard streams and host terminals do not establish a Windows console session.
	setLastError(ERROR_INVALID_HANDLE);
	return 0;
}

UINT WINAPI GetConsoleOutputCP() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetConsoleOutputCP()\n");
	setLastError(ERROR_INVALID_HANDLE);
	return 0;
}

BOOL WINAPI SetConsoleCtrlHandler(PHANDLER_ROUTINE HandlerRoutine, BOOL Add) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetConsoleCtrlHandler(%p, %u)\n", reinterpret_cast<const void *>(HandlerRoutine), Add);
	if (!HandlerRoutine) {
		g_consoleControlCIgnore.store(Add != FALSE, std::memory_order_relaxed);
		return TRUE;
	}
	if (Add) {
		auto handler =
			std::unique_ptr<ConsoleControlHandler>(new (std::nothrow) ConsoleControlHandler{HandlerRoutine, nullptr});
		if (!handler) {
			setLastError(ERROR_NOT_ENOUGH_MEMORY);
			return FALSE;
		}
		std::lock_guard lock(g_consoleControlHandlers.mutex);
		handler->next = std::move(g_consoleControlHandlers.first);
		g_consoleControlHandlers.first = std::move(handler);
		return TRUE;
	}
	std::lock_guard lock(g_consoleControlHandlers.mutex);
	auto *entry = &g_consoleControlHandlers.first;
	while (*entry && (*entry)->routine != HandlerRoutine)
		entry = &(*entry)->next;
	if (!*entry) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	auto removed = std::move(*entry);
	*entry = std::move(removed->next);
	return TRUE;
}

bool isConsoleControlCIgnored() { return g_consoleControlCIgnore.load(std::memory_order_relaxed); }

void initializeConsoleControlCIgnore(bool ignore) { g_consoleControlCIgnore.store(ignore, std::memory_order_relaxed); }

BOOL WINAPI GetConsoleScreenBufferInfo(HANDLE hConsoleOutput, CONSOLE_SCREEN_BUFFER_INFO *lpConsoleScreenBufferInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetConsoleScreenBufferInfo(%p, %p)\n", hConsoleOutput, lpConsoleScreenBufferInfo);
	return rejectUnavailableConsole(hConsoleOutput);
}

BOOL WINAPI SetConsoleTextAttribute(HANDLE hConsoleOutput, WORD wAttributes) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetConsoleTextAttribute(%p, 0x%x)\n", hConsoleOutput, wAttributes);
	return rejectUnavailableConsole(hConsoleOutput);
}

BOOL WINAPI WriteConsoleW(HANDLE hConsoleOutput, LPCWSTR lpBuffer, DWORD nNumberOfCharsToWrite,
						  LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WriteConsoleW(%p, %p, %u, %p, %p)\n", hConsoleOutput, lpBuffer, nNumberOfCharsToWrite,
			  lpNumberOfCharsWritten, lpReserved);
	if (lpNumberOfCharsWritten)
		*lpNumberOfCharsWritten = 0;
	return rejectUnavailableConsole(hConsoleOutput);
}

BOOL WINAPI ReadConsoleW(HANDLE hConsoleInput, LPVOID buffer, DWORD charsToRead, LPDWORD charsRead, LPVOID control) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ReadConsoleW(%p, %p, %u, %p, %p)\n", hConsoleInput, buffer, charsToRead, charsRead, control);
	if (charsToRead > static_cast<DWORD>(std::numeric_limits<int32_t>::max())) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	if (control) {
		CONSOLE_READCONSOLE_CONTROL parameters{};
		std::memcpy(&parameters, control, sizeof(parameters));
		if (parameters.nLength != sizeof(parameters) || parameters.nInitialChars >= charsToRead) {
			setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
	}
	if (charsRead)
		*charsRead = 0;
	return rejectUnavailableConsole(hConsoleInput);
}

DWORD WINAPI GetConsoleTitleA(LPSTR lpConsoleTitle, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetConsoleTitleA(%p, %u)\n", lpConsoleTitle, nSize);
	if (lpConsoleTitle && nSize > 0) {
		lpConsoleTitle[0] = '\0';
	}
	setLastError(ERROR_SUCCESS);
	return 0;
}

DWORD WINAPI GetConsoleTitleW(LPWSTR lpConsoleTitle, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetConsoleTitleW(%p, %u)\n", lpConsoleTitle, nSize);
	if (lpConsoleTitle && nSize > 0) {
		lpConsoleTitle[0] = 0;
	}
	setLastError(ERROR_SUCCESS);
	return 0;
}

BOOL WINAPI PeekConsoleInputA(HANDLE hConsoleInput, INPUT_RECORD *lpBuffer, DWORD nLength,
							  LPDWORD lpNumberOfEventsRead) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("PeekConsoleInputA(%p, %p, %u, %p)\n", hConsoleInput, lpBuffer, nLength, lpNumberOfEventsRead);
	return rejectUnavailableConsole(hConsoleInput);
}

BOOL WINAPI ReadConsoleInputA(HANDLE hConsoleInput, INPUT_RECORD *lpBuffer, DWORD nLength,
							  LPDWORD lpNumberOfEventsRead) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ReadConsoleInputA(%p, %p, %u, %p)\n", hConsoleInput, lpBuffer, nLength, lpNumberOfEventsRead);
	return rejectUnavailableConsole(hConsoleInput);
}

BOOL WINAPI VerifyConsoleIoHandle(HANDLE handle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: VerifyConsoleIoHandle(%p)\n", handle);
	(void)handle;
	return FALSE;
}

} // namespace kernel32
