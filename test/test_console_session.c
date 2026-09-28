#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

enum { kErrorSeed = 17185, kReportMagic = 0x43535031 };

struct Snapshot {
	UINT input;
	UINT output;
	DWORD inputError;
	DWORD outputError;
};

struct Report {
	DWORD magic;
	DWORD standardType[3];
	DWORD standardError[3];
	struct Snapshot initial;
	BOOL setInput;
	DWORD setInputError;
	struct Snapshot afterInput;
	BOOL setOutput;
	DWORD setOutputError;
	struct Snapshot afterOutput;
	BOOL restoreInput;
	DWORD restoreInputError;
	struct Snapshot afterInputRestore;
	BOOL restoreOutput;
	DWORD restoreOutputError;
	struct Snapshot final;
};

#ifndef TEST_CONSOLE_SESSION_PARENT
static struct Snapshot snapshot(void) {
	struct Snapshot result;
	SetLastError(kErrorSeed);
	result.input = GetConsoleCP();
	result.inputError = GetLastError();
	SetLastError(kErrorSeed);
	result.output = GetConsoleOutputCP();
	result.outputError = GetLastError();
	return result;
}

int wmain(int argc, WCHAR **argv) {
	if (argc != 2)
		return 2;
	struct Report report = {0};
	report.magic = kReportMagic;
	const DWORD standards[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
	for (unsigned index = 0; index < 3; ++index) {
		HANDLE handle = GetStdHandle(standards[index]);
		SetLastError(kErrorSeed);
		report.standardType[index] = GetFileType(handle);
		report.standardError[index] = GetLastError();
	}
	report.initial = snapshot();
	SetLastError(kErrorSeed);
	report.setInput = SetConsoleCP(65001);
	report.setInputError = GetLastError();
	report.afterInput = snapshot();
	SetLastError(kErrorSeed);
	report.setOutput = SetConsoleOutputCP(65001);
	report.setOutputError = GetLastError();
	report.afterOutput = snapshot();
	if (report.initial.input) {
		SetLastError(kErrorSeed);
		report.restoreInput = SetConsoleCP(report.initial.input);
		report.restoreInputError = GetLastError();
	}
	report.afterInputRestore = snapshot();
	if (report.initial.output) {
		SetLastError(kErrorSeed);
		report.restoreOutput = SetConsoleOutputCP(report.initial.output);
		report.restoreOutputError = GetLastError();
	}
	report.final = snapshot();
	HANDLE file = CreateFileW(argv[1], GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
							  FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 3;
	DWORD written = 0;
	BOOL complete = WriteFile(file, &report, sizeof(report), &written, NULL) && written == sizeof(report);
	BOOL closed = CloseHandle(file);
	return complete && closed ? 0 : 4;
}
#else
static unsigned failures;

static void check(int condition, const char *name) {
	if (!condition) {
		++failures;
		fprintf(stderr, "console session mismatch: %s\n", name);
	}
}

static void checkSnapshot(const struct Snapshot *actual, UINT input, UINT output, DWORD inputError, DWORD outputError,
						  const char *name) {
	check(actual->input == input && actual->output == output && actual->inputError == inputError &&
			  actual->outputError == outputError,
		  name);
}

static int verify(const struct Report *report, int attached, int redirected, UINT initialCodePage) {
	if (report->magic != kReportMagic)
		return 0;
	if (redirected || !attached)
		for (unsigned index = 0; index < 3; ++index) {
			const DWORD expectedType = redirected ? FILE_TYPE_CHAR : FILE_TYPE_UNKNOWN;
			const DWORD expectedError = redirected ? kErrorSeed : ERROR_INVALID_HANDLE;
			check(report->standardType[index] == expectedType && report->standardError[index] == expectedError,
				  "standard handle type");
		}
	const UINT initial = attached ? initialCodePage : 0;
	const DWORD initialError = attached ? kErrorSeed : ERROR_INVALID_HANDLE;
	checkSnapshot(&report->initial, initial, initial, initialError, initialError, "initial code pages");
	check(report->setInput == attached && report->setInputError == initialError, "set input code page");
	checkSnapshot(&report->afterInput, attached ? 65001 : 0, initial, initialError, initialError,
				  "input code page independent of output");
	check(report->setOutput == attached && report->setOutputError == initialError, "set output code page");
	checkSnapshot(&report->afterOutput, attached ? 65001 : 0, attached ? 65001 : 0, initialError, initialError,
				  "output code page independent of input");
	check(report->restoreInput == attached && report->restoreInputError == (attached ? kErrorSeed : 0),
		  "restore input code page");
	checkSnapshot(&report->afterInputRestore, initial, attached ? 65001 : 0, initialError, initialError,
				  "restored input code page");
	check(report->restoreOutput == attached && report->restoreOutputError == (attached ? kErrorSeed : 0),
		  "restore output code page");
	checkSnapshot(&report->final, initial, initial, initialError, initialError, "restored code pages");
	return 1;
}

static int runCase(const WCHAR *childPath, DWORD flags, int redirected, UINT initialCodePage) {
	WCHAR directory[MAX_PATH], reportPath[MAX_PATH], command[32768];
	DWORD length = GetTempPathW(MAX_PATH, directory);
	if (!length || length >= MAX_PATH || !GetTempFileNameW(directory, L"css", 0, reportPath))
		return 0;
	HANDLE reportFile = INVALID_HANDLE_VALUE;
	HANDLE input = INVALID_HANDLE_VALUE;
	HANDLE output = INVALID_HANDLE_VALUE;
	PROCESS_INFORMATION process = {0};
	BOOL created = FALSE;
	int good = 1;
	reportFile = CreateFileW(reportPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
							 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (reportFile == INVALID_HANDLE_VALUE) {
		good = 0;
		goto cleanup;
	}
	STARTUPINFOW startup = {0};
	startup.cb = sizeof(startup);
	if (redirected) {
		SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
		input =
			CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, 0, NULL);
		output =
			CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, 0, NULL);
		if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE) {
			good = 0;
			goto cleanup;
		}
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = input;
		startup.hStdOutput = output;
		startup.hStdError = output;
	}
	int commandLength =
		swprintf(command, sizeof(command) / sizeof(*command), L"\"%ls\" \"%ls\"", childPath, reportPath);
	if (commandLength <= 0 || (size_t)commandLength >= sizeof(command) / sizeof(*command)) {
		good = 0;
		goto cleanup;
	}
	SetLastError(kErrorSeed);
	created = CreateProcessW(childPath, command, NULL, NULL, redirected, flags, NULL, NULL, &startup, &process);
	if (!created || GetLastError() != kErrorSeed) {
		good = 0;
		goto cleanup;
	}
	DWORD waited = WaitForSingleObject(process.hProcess, 5000);
	DWORD exitCode = 0;
	if (waited != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &exitCode) || exitCode)
		good = 0;
	if (good) {
		struct Report report = {0};
		DWORD bytes = 0;
		if (!ReadFile(reportFile, &report, sizeof(report), &bytes, NULL) || bytes != sizeof(report) ||
			!verify(&report, flags != DETACHED_PROCESS, redirected, initialCodePage))
			good = 0;
	}
cleanup:
	if (created && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
		if (!TerminateProcess(process.hProcess, 8) || WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
			good = 0;
	}
	if (process.hThread && !CloseHandle(process.hThread))
		good = 0;
	if (process.hProcess && !CloseHandle(process.hProcess))
		good = 0;
	if (input != INVALID_HANDLE_VALUE && !CloseHandle(input))
		good = 0;
	if (output != INVALID_HANDLE_VALUE && !CloseHandle(output))
		good = 0;
	if (reportFile != INVALID_HANDLE_VALUE && !CloseHandle(reportFile))
		good = 0;
	if (!DeleteFileW(reportPath))
		good = 0;
	return good;
}

int wmain(int argc, WCHAR **argv) {
	if (argc != 2)
		return 2;
	SetLastError(kErrorSeed);
	check(GetConsoleCP() == 0 && GetLastError() == ERROR_INVALID_HANDLE, "GUI parent input console");
	SetLastError(kErrorSeed);
	check(GetConsoleOutputCP() == 0 && GetLastError() == ERROR_INVALID_HANDLE, "GUI parent output console");
	SetLastError(kErrorSeed);
	UINT initialCodePage = GetOEMCP();
	check(initialCodePage != 0 && GetLastError() == kErrorSeed, "initial OEM code page");
	if (!initialCodePage)
		return 1;
	check(runCase(argv[1], 0, 0, initialCodePage), "default console child");
	check(runCase(argv[1], 0, 1, initialCodePage), "redirected console child");
	check(runCase(argv[1], DETACHED_PROCESS, 0, initialCodePage), "detached console child");
	check(runCase(argv[1], CREATE_NO_WINDOW, 0, initialCodePage), "console child without window");
	printf("software_checks_failed=%u\n", failures);
	return failures ? 1 : 0;
}
#endif
