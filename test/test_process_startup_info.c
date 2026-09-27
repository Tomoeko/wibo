#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Observation {
	DWORD flagsA, flagsW, errorA, errorW;
	uint64_t inputA, outputA, errorHandleA, inputW, outputW, errorHandleW, currentOutput;
};
struct Report {
	DWORD magic;
	struct Observation before, after;
};

static void observe(struct Observation *result) {
	STARTUPINFOA narrow;
	STARTUPINFOW wide;
	memset(&narrow, 0xa5, sizeof(narrow));
	memset(&wide, 0xa5, sizeof(wide));
	SetLastError(0x4321);
	GetStartupInfoA(&narrow);
	result->errorA = GetLastError();
	SetLastError(0x4321);
	GetStartupInfoW(&wide);
	result->errorW = GetLastError();
	result->flagsA = narrow.dwFlags;
	result->flagsW = wide.dwFlags;
	result->inputA = (uint64_t)(uintptr_t)narrow.hStdInput;
	result->outputA = (uint64_t)(uintptr_t)narrow.hStdOutput;
	result->errorHandleA = (uint64_t)(uintptr_t)narrow.hStdError;
	result->inputW = (uint64_t)(uintptr_t)wide.hStdInput;
	result->outputW = (uint64_t)(uintptr_t)wide.hStdOutput;
	result->errorHandleW = (uint64_t)(uintptr_t)wide.hStdError;
	result->currentOutput = (uint64_t)(uintptr_t)GetStdHandle(STD_OUTPUT_HANDLE);
}

static int closeOwned(HANDLE *handle) {
	if (!*handle || *handle == INVALID_HANDLE_VALUE)
		return 1;
	HANDLE owned = *handle;
	*handle = NULL;
	return CloseHandle(owned) != FALSE;
}

static int childMain(const char *path, const char *alternateText) {
	uintptr_t raw = 0;
	if (!*alternateText)
		return 21;
	for (const char *cursor = alternateText; *cursor; ++cursor) {
		unsigned digit;
		if (*cursor >= '0' && *cursor <= '9')
			digit = (unsigned)(*cursor - '0');
		else if (*cursor >= 'a' && *cursor <= 'f')
			digit = (unsigned)(*cursor - 'a' + 10);
		else
			return 21;
		if (raw > (UINTPTR_MAX - digit) / 16)
			return 21;
		raw = raw * 16 + digit;
	}
	HANDLE alternate = (HANDLE)raw;
	if (GetFileType(alternate) != FILE_TYPE_DISK)
		return 22;
	struct Report report = {0};
	report.magic = 0x53544931;
	observe(&report.before);
	HANDLE original = GetStdHandle(STD_OUTPUT_HANDLE);
	if (!SetStdHandle(STD_OUTPUT_HANDLE, alternate))
		return 23;
	observe(&report.after);
	if (!SetStdHandle(STD_OUTPUT_HANDLE, original))
		return 24;
	HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
							  FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 25;
	DWORD written = 0;
	BOOL result = WriteFile(file, &report, sizeof(report), &written, NULL);
	BOOL closed = CloseHandle(file);
	return result && written == sizeof(report) && closed ? 0 : 26;
}

static void printObservation(const char *name, const struct Observation *value) {
	printf(" %s flagsA=%lx flagsW=%lx LastErrorA=%lu LastErrorW=%lu A=%llx,%llx,%llx W=%llx,%llx,%llx current=%llx\n",
		   name, (unsigned long)value->flagsA, (unsigned long)value->flagsW, (unsigned long)value->errorA,
		   (unsigned long)value->errorW, (unsigned long long)value->inputA, (unsigned long long)value->outputA,
		   (unsigned long long)value->errorHandleA, (unsigned long long)value->inputW,
		   (unsigned long long)value->outputW, (unsigned long long)value->errorHandleW,
		   (unsigned long long)value->currentOutput);
}

static int runCase(BOOL explicitStandards) {
	int result = 1;
	char temporary[MAX_PATH], paths[3][MAX_PATH] = {{0}}, image[32768], command[32768 + MAX_PATH + 128];
	HANDLE standard = INVALID_HANDLE_VALUE, alternate = INVALID_HANDLE_VALUE, reader = INVALID_HANDLE_VALUE;
	PROCESS_INFORMATION process = {0};
	STARTUPINFOA startup = {0};
	SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
	DWORD length = GetTempPathA(sizeof(temporary), temporary);
	if (!length || length >= sizeof(temporary))
		goto cleanup;
	for (unsigned index = 0; index < 3; ++index)
		if (!GetTempFileNameA(temporary, "sti", 0, paths[index]))
			goto cleanup;
	standard = CreateFileA(paths[0], GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
						   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	alternate = CreateFileA(paths[1], GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
							OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	length = GetModuleFileNameA(NULL, image, sizeof(image));
	if (standard == INVALID_HANDLE_VALUE || alternate == INVALID_HANDLE_VALUE || !length || length >= sizeof(image))
		goto cleanup;
	int count = snprintf(command, sizeof(command), "\"%s\" child \"%s\" %llx", image, paths[2],
						 (unsigned long long)(uintptr_t)alternate);
	if (count <= 0 || (size_t)count >= sizeof(command))
		goto cleanup;
	startup.cb = sizeof(startup);
	if (explicitStandards) {
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = standard;
		startup.hStdOutput = standard;
		startup.hStdError = standard;
	}
	if (!CreateProcessA(image, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process))
		goto cleanup;
	DWORD waited = WaitForSingleObject(process.hProcess, 5000), code = 0;
	if (waited != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &code) || code)
		goto cleanup;
	reader = CreateFileA(paths[2], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
						 FILE_ATTRIBUTE_NORMAL, NULL);
	struct Report report;
	DWORD read = 0;
	if (reader == INVALID_HANDLE_VALUE || !ReadFile(reader, &report, sizeof(report), &read, NULL) ||
		read != sizeof(report) || report.magic != 0x53544931)
		goto cleanup;
	printf("case explicit=%u original=%llx alternate=%llx\n", (unsigned)explicitStandards,
		   (unsigned long long)(uintptr_t)standard, (unsigned long long)(uintptr_t)alternate);
	printObservation("before", &report.before);
	printObservation("after", &report.after);
	const DWORD expectedFlag = explicitStandards ? STARTF_USESTDHANDLES : 0;
	if (report.before.flagsA != expectedFlag || report.before.flagsW != expectedFlag ||
		report.after.flagsA != expectedFlag || report.after.flagsW != expectedFlag || report.before.errorA != 0x4321 ||
		report.before.errorW != 0x4321 || report.after.errorA != 0x4321 || report.after.errorW != 0x4321 ||
		report.after.currentOutput != (uint64_t)(uintptr_t)alternate)
		goto cleanup;
	if (explicitStandards) {
		const uint64_t original = (uint64_t)(uintptr_t)standard;
		if (report.before.inputA != original || report.before.outputA != original ||
			report.before.errorHandleA != original || report.before.inputW != original ||
			report.before.outputW != original || report.before.errorHandleW != original ||
			report.before.currentOutput != original || report.after.inputW != original ||
			report.after.outputW != (uint64_t)(uintptr_t)alternate || report.after.errorHandleW != original)
			goto cleanup;
		// ANSI values after mutation are recorded separately; its cache behavior is not a portable assertion.
	} else {
		const uint64_t canary = (uint64_t)(UINTPTR_MAX / 0xff * 0xa5);
		if (report.before.inputW != canary || report.before.outputW != canary || report.before.errorHandleW != canary ||
			report.after.inputW != canary || report.after.outputW != canary || report.after.errorHandleW != canary)
			goto cleanup;
	}
	result = 0;
cleanup:
	if (result)
		fprintf(stderr, "startup observation failed error=%lu pid=%lu\n", (unsigned long)GetLastError(),
				(unsigned long)process.dwProcessId);
	if (process.hProcess && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
		BOOL terminated = TerminateProcess(process.hProcess, 101);
		DWORD reaped = WaitForSingleObject(process.hProcess, 5000);
		fprintf(stderr, "cleanup terminated=%u wait=%lu\n", (unsigned)terminated, (unsigned long)reaped);
		if (!terminated || reaped != WAIT_OBJECT_0)
			result = 1;
	}
	int threadClosed = closeOwned(&process.hThread), processClosed = closeOwned(&process.hProcess);
	int standardClosed = closeOwned(&standard), alternateClosed = closeOwned(&alternate),
		readerClosed = closeOwned(&reader);
	int deleted = 1;
	for (unsigned index = 0; index < 3; ++index)
		if (paths[index][0] && !DeleteFileA(paths[index]))
			deleted = 0;
	return result || !threadClosed || !processClosed || !standardClosed || !alternateClosed || !readerClosed ||
		   !deleted;
}

int main(int argc, char **argv) {
	if (argc == 4 && !strcmp(argv[1], "child"))
		return childMain(argv[2], argv[3]);
	if (argc != 1)
		return 2;
	struct Observation initial;
	observe(&initial);
	printObservation("initial", &initial);
	return runCase(FALSE) || runCase(TRUE);
}
