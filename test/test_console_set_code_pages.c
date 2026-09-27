#define _WIN32_WINNT 0x0600
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

enum { kErrorSeed = 17185 };

typedef BOOL(WINAPI *ConsoleSessionOperation)(void);

static HANDLE reportFile;
static int reportFailed;

static void record(const char *phase, const char *operation, UINT input, DWORD result, DWORD error) {
	char line[160];
	int length = snprintf(line, sizeof(line), "%s %s input=%lu result=%lu error=%lu\n", phase, operation,
						  (unsigned long)input, (unsigned long)result, (unsigned long)error);
	DWORD written = 0;
	if (length <= 0 || (size_t)length >= sizeof(line) || !WriteFile(reportFile, line, (DWORD)length, &written, NULL) ||
		written != (DWORD)length)
		reportFailed = 1;
}

static UINT queryInput(const char *phase) {
	SetLastError(kErrorSeed);
	UINT value = GetConsoleCP();
	DWORD error = GetLastError();
	record(phase, "GetConsoleCP", 0, value, error);
	return value;
}

static UINT queryOutput(const char *phase) {
	SetLastError(kErrorSeed);
	UINT value = GetConsoleOutputCP();
	DWORD error = GetLastError();
	record(phase, "GetConsoleOutputCP", 0, value, error);
	return value;
}

static void setInput(const char *phase, UINT codePage) {
	SetLastError(kErrorSeed);
	BOOL result = SetConsoleCP(codePage);
	DWORD error = GetLastError();
	record(phase, "SetConsoleCP", codePage, (DWORD)result, error);
	queryInput(phase);
	queryOutput(phase);
}

static void setOutput(const char *phase, UINT codePage) {
	SetLastError(kErrorSeed);
	BOOL result = SetConsoleOutputCP(codePage);
	DWORD error = GetLastError();
	record(phase, "SetConsoleOutputCP", codePage, (DWORD)result, error);
	queryInput(phase);
	queryOutput(phase);
}

static void setBoth(const char *phase, UINT codePage) {
	setInput(phase, codePage);
	setOutput(phase, codePage);
}

static int detachedChild(void) {
	const UINT codePages[] = {0, 437, 65001};
	for (unsigned index = 0; index < sizeof(codePages) / sizeof(*codePages); ++index) {
		SetLastError(kErrorSeed);
		if (GetConsoleCP() != 0 || GetLastError() != ERROR_INVALID_HANDLE)
			return 20;
		SetLastError(kErrorSeed);
		if (GetConsoleOutputCP() != 0 || GetLastError() != ERROR_INVALID_HANDLE)
			return 21;
		SetLastError(kErrorSeed);
		if (SetConsoleCP(codePages[index]) || GetLastError() != ERROR_INVALID_HANDLE)
			return 22;
		SetLastError(kErrorSeed);
		if (SetConsoleOutputCP(codePages[index]) || GetLastError() != ERROR_INVALID_HANDLE)
			return 23;
	}
	return 0;
}

static int detachedParent(void) {
	WCHAR executable[32768], command[32768];
	DWORD length = GetModuleFileNameW(NULL, executable, sizeof(executable) / sizeof(*executable));
	if (!length || length >= sizeof(executable) / sizeof(*executable))
		return 30;
	int characters = swprintf(command, sizeof(command) / sizeof(*command), L"\"%ls\" --assert-child", executable);
	if (characters < 0 || (size_t)characters >= sizeof(command) / sizeof(*command))
		return 31;
	STARTUPINFOW startup = {0};
	PROCESS_INFORMATION process = {0};
	startup.cb = sizeof(startup);
	if (!CreateProcessW(executable, command, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &startup, &process))
		return 32;
	int result = 0;
	if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
		result = 33;
	DWORD childResult = 0;
	if (!result && (!GetExitCodeProcess(process.hProcess, &childResult) || childResult != 0))
		result = childResult ? (int)childResult : 34;
	if (result && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0 &&
		(!TerminateProcess(process.hProcess, 35) || WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0))
		result = 35;
	BOOL threadClosed = CloseHandle(process.hThread);
	BOOL processClosed = CloseHandle(process.hProcess);
	if (!threadClosed || !processClosed)
		result = 36;
	return result;
}

static int observe(const WCHAR *reportPath) {
	reportFile = CreateFileW(reportPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (reportFile == INVALID_HANDLE_VALUE)
		return 3;

	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	FARPROC allocateAddress = kernel ? GetProcAddress(kernel, "AllocConsole") : NULL;
	FARPROC freeAddress = kernel ? GetProcAddress(kernel, "FreeConsole") : NULL;
	ConsoleSessionOperation allocateConsole = NULL, freeConsole = NULL;
	memcpy(&allocateConsole, &allocateAddress, sizeof(allocateConsole));
	memcpy(&freeConsole, &freeAddress, sizeof(freeConsole));
	UINT originalInput = queryInput("initial");
	UINT originalOutput = queryOutput("initial");
	if (!originalInput && !originalOutput && allocateConsole) {
		SetLastError(kErrorSeed);
		BOOL result = allocateConsole();
		DWORD error = GetLastError();
		record("attach", "AllocConsole", 0, (DWORD)result, error);
	}
	UINT attachedInput = queryInput("attached_before");
	UINT attachedOutput = queryOutput("attached_before");
	if (attachedInput || attachedOutput) {
		setBoth("attached_invalid", 0);
		setBoth("attached_oem", 437);
		setBoth("attached_utf8", 65001);
		if (attachedInput)
			setInput("restore", attachedInput);
		if (attachedOutput)
			setOutput("restore", attachedOutput);
	}
	if (freeConsole) {
		SetLastError(kErrorSeed);
		BOOL result = freeConsole();
		DWORD error = GetLastError();
		record("detach", "FreeConsole", 0, (DWORD)result, error);
	}
	UINT detachedInput = queryInput("after_free");
	UINT detachedOutput = queryOutput("after_free");
	if (!detachedInput && !detachedOutput) {
		setBoth("no_console_invalid", 0);
		setBoth("no_console_oem", 437);
		setBoth("no_console_utf8", 65001);
	}
	if (!CloseHandle(reportFile))
		return 4;
	return reportFailed ? 5 : 0;
}

int wmain(int argc, WCHAR **argv) {
	if (argc == 1)
		return detachedParent();
	if (argc == 2 && wcscmp(argv[1], L"--assert-child") == 0)
		return detachedChild();
	if (argc == 2)
		return observe(argv[1]);
	return 2;
}
