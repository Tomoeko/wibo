#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

static const WCHAR supplementaryName[] = L"ENV_PROBE_\xD83D\xDE42";
static const WCHAR supplementaryValue[] = L"face:\xD83D\xDE42";
static const char ansiEnvironment[] = "ENV_PROBE_EMPTY=\0"
									  "ENV_PROBE_KIND=ansi\0"
									  "ENV_PROBE_VALUE=explicit\0";
static const WCHAR unicodeEnvironment[] = L"ENV_PROBE_EMPTY=\0"
										  L"ENV_PROBE_KIND=unicode\0"
										  L"ENV_PROBE_UNICODE=face:\xD83D\xDE42\0"
										  L"ENV_PROBE_VALUE=explicit\0"
										  L"ENV_PROBE_\xD83D\xDE42=face:\xD83D\xDE42\0";
static const WCHAR driveEnvironment[] = L"=C:=C:\\Fixture\0"
										L"ENV_PROBE_KIND=drive\0";

static int valueIs(const WCHAR *name, const WCHAR *expected) {
	WCHAR value[128];
	value[0] = 0xA5;
	SetLastError(0);
	DWORD length = GetEnvironmentVariableW(name, value, sizeof(value) / sizeof(*value));
	return length == wcslen(expected) && length < sizeof(value) / sizeof(*value) && value[length] == 0 &&
		   !memcmp(value, expected, (length + 1) * sizeof(*value)) && GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

static int missing(const WCHAR *name) {
	WCHAR value[32];
	SetLastError(0);
	return GetEnvironmentVariableW(name, value, sizeof(value) / sizeof(*value)) == 0 &&
		   GetLastError() == ERROR_ENVVAR_NOT_FOUND;
}

static int childMain(const char *mode) {
	int inherited = !strcmp(mode, "inherited");
	int unicode = !strcmp(mode, "unicode");
	if (!inherited && !unicode && strcmp(mode, "ansi"))
		return 20;
	if (!valueIs(L"ENV_PROBE_EMPTY", L"") ||
		!valueIs(L"ENV_PROBE_KIND", inherited ? L"inherited"
									: unicode ? L"unicode"
											  : L"ansi") ||
		!valueIs(L"ENV_PROBE_VALUE", inherited ? L"parent" : L"explicit") || !missing(L"ENV_PROBE_CHILD_ONLY"))
		return 21;
	if (inherited) {
		if (!valueIs(L"ENV_PROBE_PARENT_ONLY", L"present"))
			return 22;
	} else if (!missing(L"ENV_PROBE_PARENT_ONLY")) {
		return 23;
	}
	if (inherited || unicode) {
		if (!valueIs(L"ENV_PROBE_UNICODE", supplementaryValue) || !valueIs(supplementaryName, supplementaryValue))
			return 24;
	} else if (!missing(L"ENV_PROBE_UNICODE") || !missing(supplementaryName)) {
		return 25;
	}
	if (!SetEnvironmentVariableW(L"ENV_PROBE_VALUE", L"child") ||
		!SetEnvironmentVariableW(L"ENV_PROBE_PARENT_ONLY", NULL) ||
		!SetEnvironmentVariableW(L"ENV_PROBE_UNICODE", NULL) ||
		!SetEnvironmentVariableW(L"ENV_PROBE_CHILD_ONLY", L"child") || !valueIs(L"ENV_PROBE_VALUE", L"child") ||
		!valueIs(L"ENV_PROBE_CHILD_ONLY", L"child") || !missing(L"ENV_PROBE_PARENT_ONLY") ||
		!missing(L"ENV_PROBE_UNICODE"))
		return 26;
	return 0;
}

static int observeDrive(void) {
	WCHAR *environment = GetEnvironmentStringsW();
	if (!environment)
		return 74;
	int found = 0;
	for (WCHAR *entry = environment; *entry; entry += wcslen(entry) + 1) {
		if (!wcscmp(entry, L"=C:=C:\\Fixture"))
			found = 1;
	}
	if (!FreeEnvironmentStringsW(environment))
		return 75;
	return found ? 0 : 73;
}

static int closeOwned(HANDLE *handle) {
	if (!*handle || *handle == INVALID_HANDLE_VALUE)
		return 1;
	HANDLE value = *handle;
	*handle = NULL;
	return CloseHandle(value) != FALSE;
}

static int runCase(BOOL wideApi, DWORD flags, const void *environment, const char *mode, BOOL observational) {
	char imageA[32768], commandA[32768 + 64];
	WCHAR imageW[32768], commandW[32768 + 64];
	PROCESS_INFORMATION process = {0};
	STARTUPINFOA startupA = {0};
	STARTUPINFOW startupW = {0};
	startupA.cb = sizeof(startupA);
	startupW.cb = sizeof(startupW);
	BOOL created = FALSE;
	if (wideApi) {
		DWORD length = GetModuleFileNameW(NULL, imageW, sizeof(imageW) / sizeof(*imageW));
		if (!length || length >= sizeof(imageW) / sizeof(*imageW))
			return 1;
		WCHAR wideMode[32];
		size_t modeLength = strlen(mode);
		if (modeLength >= sizeof(wideMode) / sizeof(*wideMode))
			return 1;
		for (size_t index = 0; index <= modeLength; ++index)
			wideMode[index] = (WCHAR)(unsigned char)mode[index];
		int formatted =
			swprintf(commandW, sizeof(commandW) / sizeof(*commandW), L"\"%ls\" --child %ls", imageW, wideMode);
		if (formatted <= 0 || (size_t)formatted >= sizeof(commandW) / sizeof(*commandW))
			return 1;
		created =
			CreateProcessW(imageW, commandW, NULL, NULL, FALSE, flags, (LPVOID)environment, NULL, &startupW, &process);
	} else {
		DWORD length = GetModuleFileNameA(NULL, imageA, sizeof(imageA));
		if (!length || length >= sizeof(imageA))
			return 1;
		int formatted = snprintf(commandA, sizeof(commandA), "\"%s\" --child %s", imageA, mode);
		if (formatted <= 0 || (size_t)formatted >= sizeof(commandA))
			return 1;
		created =
			CreateProcessA(imageA, commandA, NULL, NULL, FALSE, flags, (LPVOID)environment, NULL, &startupA, &process);
	}
	if (!created) {
		fprintf(stderr, "CreateProcess%c mode=%s flags=%lu failed error=%lu\n", wideApi ? 'W' : 'A', mode,
				(unsigned long)flags, (unsigned long)GetLastError());
		return 1;
	}
	DWORD wait = WaitForSingleObject(process.hProcess, 5000);
	DWORD code = STILL_ACTIVE;
	BOOL queried = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code);
	BOOL cleaned = TRUE;
	if (wait != WAIT_OBJECT_0) {
		BOOL terminated = TerminateProcess(process.hProcess, 97);
		DWORD reaped = WaitForSingleObject(process.hProcess, 5000);
		fprintf(stderr, "Cleanup pid=%lu terminate=%d reap=%lu\n", (unsigned long)process.dwProcessId, terminated,
				(unsigned long)reaped);
		cleaned = terminated && reaped == WAIT_OBJECT_0;
	}
	int threadClosed = closeOwned(&process.hThread);
	int processClosed = closeOwned(&process.hProcess);
	printf("api=%c mode=%s flags=%lu wait=%lu result=%lu observational=%u\n", wideApi ? 'W' : 'A', mode,
		   (unsigned long)flags, (unsigned long)wait, (unsigned long)code, (unsigned)observational);
	int accepted = observational ? code == 0 || code == 73 : code == 0;
	return !queried || !accepted || !cleaned || !threadClosed || !processClosed;
}

static int parentUnchanged(void) {
	return valueIs(L"ENV_PROBE_EMPTY", L"") && valueIs(L"ENV_PROBE_KIND", L"inherited") &&
		   valueIs(L"ENV_PROBE_VALUE", L"parent") && valueIs(L"ENV_PROBE_PARENT_ONLY", L"present") &&
		   valueIs(L"ENV_PROBE_UNICODE", supplementaryValue) && valueIs(supplementaryName, supplementaryValue) &&
		   missing(L"ENV_PROBE_CHILD_ONLY");
}

int main(int argc, char **argv) {
	if (argc == 3 && !strcmp(argv[1], "--child"))
		return !strcmp(argv[2], "drive") ? observeDrive() : childMain(argv[2]);
	if (argc != 1)
		return 2;
	if (!SetEnvironmentVariableW(L"ENV_PROBE_EMPTY", L"") ||
		!SetEnvironmentVariableW(L"ENV_PROBE_KIND", L"inherited") ||
		!SetEnvironmentVariableW(L"ENV_PROBE_VALUE", L"parent") ||
		!SetEnvironmentVariableW(L"ENV_PROBE_PARENT_ONLY", L"present") ||
		!SetEnvironmentVariableW(L"ENV_PROBE_UNICODE", supplementaryValue) ||
		!SetEnvironmentVariableW(supplementaryName, supplementaryValue) ||
		!SetEnvironmentVariableW(L"ENV_PROBE_CHILD_ONLY", NULL))
		return 3;
	int failed = 0;
	for (unsigned api = 0; api < 2; ++api) {
		failed |= runCase(api, 0, ansiEnvironment, "ansi", FALSE);
		failed |= !parentUnchanged();
		failed |= runCase(api, CREATE_UNICODE_ENVIRONMENT, unicodeEnvironment, "unicode", FALSE);
		failed |= !parentUnchanged();
		failed |= runCase(api, api ? CREATE_UNICODE_ENVIRONMENT : 0, NULL, "inherited", FALSE);
		failed |= !parentUnchanged();
		failed |= runCase(api, CREATE_UNICODE_ENVIRONMENT, driveEnvironment, "drive", TRUE);
		failed |= !parentUnchanged();
	}
	return failed != 0;
}
