#include "wincon.h"

#include "context.h"
#include "errors.h"
#include "files.h"
#include "handles.h"
#include "system_provider.h"
#include "winnls.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

constexpr uint32_t kConsoleSessionMagic = 0x434E534C;
constexpr uint32_t kConsoleSessionVersion = 1;
constexpr uint16_t kConsoleSubsystem = 3;

struct ConsoleSharedState {
	uint32_t magic;
	uint32_t version;
	uint32_t inputCodePage;
	uint32_t outputCodePage;
};
static_assert(sizeof(ConsoleSharedState) == 16);
static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr));

struct ConsoleSession {
	std::mutex mutex;
	int descriptor = -1;
	ConsoleSharedState *state = nullptr;
	~ConsoleSession() {
		if (state)
			munmap(state, sizeof(ConsoleSharedState));
		if (descriptor >= 0)
			close(descriptor);
	}
};

ConsoleSession g_consoleSession;
std::atomic<ConsoleSharedState *> g_consoleState{nullptr};

UINT initialConsoleCodePage() {
	if (wibo::provider::configured()) {
		const DWORD previousError = getLastError();
		const UINT oemCodePage = GetOEMCP();
		setLastError(previousError);
		return oemCodePage;
	}
	// Default emulated OEM page without a native code-page source. Other system
	// defaults remain unsupported by this fallback.
	return 437;
}

DWORD codePageError(UINT codePage) {
	if (!codePage)
		return ERROR_INVALID_PARAMETER;
	if (codePage == 437 || codePage == 65001)
		return 0;
	if (codePage <= 2 || !wibo::provider::configured())
		return ERROR_NOT_SUPPORTED;
	CPINFOEXW information{};
	if (!GetCPInfoExW(codePage, 0, &information)) {
		const DWORD error = getLastError();
		return error ? error : ERROR_NOT_SUPPORTED;
	}
	return information.CodePage == codePage ? 0 : ERROR_NOT_SUPPORTED;
}

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

DWORD initializeConsoleSession(uint16_t imageSubsystem, bool detached, int inheritedDescriptor) {
	const bool consoleImage = imageSubsystem == kConsoleSubsystem;
	if (detached || !consoleImage) {
		if (inheritedDescriptor >= 0)
			close(inheritedDescriptor);
		return 0;
	}
	std::lock_guard lock(g_consoleSession.mutex);
	if (g_consoleSession.state) {
		if (inheritedDescriptor >= 0)
			close(inheritedDescriptor);
		return ERROR_INVALID_PARAMETER;
	}
	int descriptor = inheritedDescriptor;
	bool created = descriptor < 0;
	if (created) {
		char name[] = "/tmp/wibo-console-XXXXXX";
		descriptor = mkostemp(name, O_CLOEXEC);
		if (descriptor < 0)
			return wibo::winErrorFromErrno(errno);
		if (unlink(name) != 0 || ftruncate(descriptor, sizeof(ConsoleSharedState)) != 0) {
			const DWORD error = wibo::winErrorFromErrno(errno);
			close(descriptor);
			return error;
		}
	}
	struct stat info{};
	if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size != sizeof(ConsoleSharedState)) {
		close(descriptor);
		return ERROR_INVALID_DATA;
	}
	void *mapped = mmap(nullptr, sizeof(ConsoleSharedState), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
	if (mapped == MAP_FAILED) {
		const DWORD error = wibo::winErrorFromErrno(errno);
		close(descriptor);
		return error;
	}
	auto *state = static_cast<ConsoleSharedState *>(mapped);
	if (created) {
		const UINT codePage = initialConsoleCodePage();
		*state = {kConsoleSessionMagic, kConsoleSessionVersion, codePage, codePage};
	} else if (state->magic != kConsoleSessionMagic || state->version != kConsoleSessionVersion) {
		munmap(mapped, sizeof(ConsoleSharedState));
		close(descriptor);
		return ERROR_INVALID_DATA;
	}
	g_consoleSession.descriptor = descriptor;
	g_consoleSession.state = state;
	g_consoleState.store(state, std::memory_order_release);
	DEBUG_LOG("Console session initialized (inherited=%u, input=%u, output=%u)\n", !created,
			  state->inputCodePage, state->outputCodePage);
	return 0;
}

DWORD snapshotConsoleSessionDescriptor(int &descriptor) {
	descriptor = -1;
	std::lock_guard lock(g_consoleSession.mutex);
	if (g_consoleSession.descriptor < 0)
		return 0;
	descriptor = fcntl(g_consoleSession.descriptor, F_DUPFD_CLOEXEC, 3);
	return descriptor < 0 ? wibo::winErrorFromErrno(errno) : 0;
}

bool hasConsoleSession() { return g_consoleState.load(std::memory_order_acquire) != nullptr; }

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
	// Attaching to a different process requires an explicit session lookup and
	// reference transfer; process creation only carries inherited sessions.
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
	ConsoleSharedState *state = g_consoleState.load(std::memory_order_acquire);
	if (state) {
		const UINT value = __atomic_load_n(&state->inputCodePage, __ATOMIC_ACQUIRE);
		if (value)
			return value;
		setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	setLastError(ERROR_INVALID_HANDLE);
	return 0;
}

UINT WINAPI GetConsoleOutputCP() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetConsoleOutputCP()\n");
	ConsoleSharedState *state = g_consoleState.load(std::memory_order_acquire);
	if (state) {
		const UINT value = __atomic_load_n(&state->outputCodePage, __ATOMIC_ACQUIRE);
		if (value)
			return value;
		setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	setLastError(ERROR_INVALID_HANDLE);
	return 0;
}

BOOL WINAPI SetConsoleCP(UINT codePage) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetConsoleCP(%u)\n", codePage);
	ConsoleSharedState *state = g_consoleState.load(std::memory_order_acquire);
	if (!state) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	const DWORD incomingError = getLastError();
	const DWORD error = codePageError(codePage);
	if (error) {
		setLastError(error);
		return FALSE;
	}
	__atomic_store_n(&state->inputCodePage, codePage, __ATOMIC_RELEASE);
	setLastError(incomingError);
	return TRUE;
}

BOOL WINAPI SetConsoleOutputCP(UINT codePage) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetConsoleOutputCP(%u)\n", codePage);
	ConsoleSharedState *state = g_consoleState.load(std::memory_order_acquire);
	if (!state) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	const DWORD incomingError = getLastError();
	const DWORD error = codePageError(codePage);
	if (error) {
		setLastError(error);
		return FALSE;
	}
	__atomic_store_n(&state->outputCodePage, codePage, __ATOMIC_RELEASE);
	setLastError(incomingError);
	return TRUE;
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
