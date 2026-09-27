#include "wincon.h"

#include "context.h"
#include "errors.h"
#include "files.h"
#include "handles.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <limits>

namespace kernel32 {

namespace {
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
	DEBUG_LOG("STUB: GetConsoleCP() -> 65001\n");
	return 65001; // UTF-8
}

UINT WINAPI GetConsoleOutputCP() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetConsoleOutputCP() -> 65001\n");
	return 65001; // UTF-8
}

BOOL WINAPI SetConsoleCtrlHandler(PHANDLER_ROUTINE HandlerRoutine, BOOL Add) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetConsoleCtrlHandler(%p, %u)\n", reinterpret_cast<const void *>(HandlerRoutine), Add);
	(void)HandlerRoutine;
	(void)Add;
	return TRUE;
}

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
