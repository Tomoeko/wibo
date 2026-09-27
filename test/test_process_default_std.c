#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct FileIdentity {
	DWORD volume;
	DWORD high;
	DWORD low;
};

struct StandardObservation {
	uint64_t handle;
	DWORD type;
	DWORD typeError;
	DWORD flagsResult;
	DWORD flags;
	DWORD flagsError;
	DWORD identityResult;
	DWORD identityError;
	DWORD identityMatches;
	DWORD operationResult;
	DWORD operationError;
	DWORD bytes;
	unsigned char data[4];
};

struct Report {
	DWORD magic;
	DWORD startupFlags;
	DWORD deniedWrite;
	DWORD deniedWriteError;
	struct StandardObservation standard[3];
};

static int closeOwned(HANDLE *handle) {
	if (!*handle || *handle == INVALID_HANDLE_VALUE)
		return 1;
	HANDLE value = *handle;
	*handle = NULL;
	return CloseHandle(value) != FALSE;
}

static int parseHex(const char *text, DWORD *result) {
	if (strlen(text) != 8)
		return 0;
	DWORD value = 0;
	for (unsigned index = 0; index < 8; ++index) {
		unsigned digit;
		if (text[index] >= '0' && text[index] <= '9')
			digit = (unsigned)(text[index] - '0');
		else if (text[index] >= 'a' && text[index] <= 'f')
			digit = (unsigned)(text[index] - 'a' + 10);
		else
			return 0;
		value = (value << 4) | digit;
	}
	*result = value;
	return 1;
}

static int identity(HANDLE file, struct FileIdentity *result) {
	BY_HANDLE_FILE_INFORMATION information;
	if (!GetFileInformationByHandle(file, &information))
		return 0;
	result->volume = information.dwVolumeSerialNumber;
	result->high = information.nFileIndexHigh;
	result->low = information.nFileIndexLow;
	return 1;
}

static void observeStandard(DWORD standard, const struct FileIdentity *expected, BOOL reading,
							struct StandardObservation *observation) {
	HANDLE handle = GetStdHandle(standard);
	observation->handle = (uint64_t)(uintptr_t)handle;
	SetLastError(0x4321);
	observation->type = GetFileType(handle);
	observation->typeError = GetLastError();
	SetLastError(0x4321);
	observation->flagsResult = GetHandleInformation(handle, &observation->flags);
	observation->flagsError = GetLastError();
	struct FileIdentity actual = {0};
	SetLastError(0x4321);
	observation->identityResult = identity(handle, &actual);
	observation->identityError = GetLastError();
	observation->identityMatches = observation->identityResult && actual.volume == expected->volume &&
								   actual.high == expected->high && actual.low == expected->low;
	// A copied numeric value can identify a different child object; only touch the owned file.
	if (!observation->identityMatches)
		return;
	SetLastError(0x4321);
	if (reading) {
		observation->operationResult = ReadFile(handle, observation->data, 2, &observation->bytes, NULL);
	} else {
		const unsigned char *bytes = (const unsigned char *)(standard == STD_OUTPUT_HANDLE ? "out" : "err");
		observation->operationResult = WriteFile(handle, bytes, 3, &observation->bytes, NULL);
		if (observation->operationResult)
			memcpy(observation->data, bytes, 3);
	}
	observation->operationError = GetLastError();
}

static int childMain(char **arguments) {
	struct FileIdentity input, output;
	if (!parseHex(arguments[3], &input.volume) || !parseHex(arguments[4], &input.high) ||
		!parseHex(arguments[5], &input.low) || !parseHex(arguments[6], &output.volume) ||
		!parseHex(arguments[7], &output.high) || !parseHex(arguments[8], &output.low))
		return 21;
	struct Report report = {0};
	report.magic = 0x53544431;
	STARTUPINFOA startup;
	GetStartupInfoA(&startup);
	report.startupFlags = startup.dwFlags;
	observeStandard(STD_INPUT_HANDLE, &input, TRUE, &report.standard[0]);
	observeStandard(STD_OUTPUT_HANDLE, &output, FALSE, &report.standard[1]);
	observeStandard(STD_ERROR_HANDLE, &output, FALSE, &report.standard[2]);
	if (report.standard[0].identityMatches) {
		DWORD written = 0;
		SetLastError(0x4321);
		report.deniedWrite = WriteFile(GetStdHandle(STD_INPUT_HANDLE), "x", 1, &written, NULL);
		report.deniedWriteError = GetLastError();
	}
	HANDLE file = CreateFileA(arguments[2], GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
							  FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 22;
	DWORD written = 0;
	BOOL result = WriteFile(file, &report, sizeof(report), &written, NULL);
	BOOL closed = CloseHandle(file);
	return result && written == sizeof(report) && closed ? 0 : 23;
}

static int position(HANDLE file, LONGLONG value, DWORD method, LONGLONG *result) {
	LARGE_INTEGER offset, actual;
	offset.QuadPart = value;
	if (!SetFilePointerEx(file, offset, &actual, method))
		return 0;
	if (result)
		*result = actual.QuadPart;
	return 1;
}

static int readReport(const char *path, struct Report *report) {
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
							  FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	unsigned char bytes[sizeof(*report) + 1];
	DWORD read = 0;
	BOOL result = ReadFile(file, bytes, sizeof(bytes), &read, NULL);
	BOOL closed = CloseHandle(file);
	if (!result || !closed || read != sizeof(*report))
		return 0;
	memcpy(report, bytes, sizeof(*report));
	return report->magic == 0x53544431;
}

static int runCase(BOOL inputInherit, BOOL outputInherit, BOOL inherit) {
	int result = 1;
	char temporary[MAX_PATH], inputPath[MAX_PATH] = {0}, outputPath[MAX_PATH] = {0}, reportPath[MAX_PATH] = {0};
	char image[32768], command[32768 + 3 * MAX_PATH + 128];
	HANDLE input = INVALID_HANDLE_VALUE, output = INVALID_HANDLE_VALUE, extra = INVALID_HANDLE_VALUE;
	HANDLE saved[3] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
	const DWORD standards[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
	STARTUPINFOA startup = {0};
	PROCESS_INFORMATION child = {0};
	unsigned standardsChanged = 0;
	BOOL created = FALSE;
	DWORD creationError = 0, exitCode = 0;
	struct Report report = {0};
	LONGLONG inputPosition = -1, outputPosition = -1;
	startup.cb = sizeof(startup);
	DWORD length = GetTempPathA(sizeof(temporary), temporary);
	if (!length || length >= sizeof(temporary) || !GetTempFileNameA(temporary, "dsi", 0, inputPath) ||
		!GetTempFileNameA(temporary, "dso", 0, outputPath) || !GetTempFileNameA(temporary, "dsr", 0, reportPath))
		goto cleanup;
	input = CreateFileA(inputPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
						OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	output = CreateFileA(outputPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
						 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	extra = CreateFileA(reportPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
						OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE || extra == INVALID_HANDLE_VALUE)
		goto cleanup;
	DWORD written = 0;
	if (!WriteFile(input, "0123456789", 10, &written, NULL) || written != 10 || !position(input, 3, FILE_BEGIN, NULL) ||
		!SetHandleInformation(input, HANDLE_FLAG_INHERIT, inputInherit ? HANDLE_FLAG_INHERIT : 0) ||
		!SetHandleInformation(output, HANDLE_FLAG_INHERIT, outputInherit ? HANDLE_FLAG_INHERIT : 0) ||
		!SetHandleInformation(extra, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
		goto cleanup;
	if (!closeOwned(&input))
		goto cleanup;
	input = CreateFileA(inputPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
						FILE_ATTRIBUTE_NORMAL, NULL);
	if (input == INVALID_HANDLE_VALUE || !position(input, 3, FILE_BEGIN, NULL) ||
		!SetHandleInformation(input, HANDLE_FLAG_INHERIT, inputInherit ? HANDLE_FLAG_INHERIT : 0))
		goto cleanup;
	struct FileIdentity inputIdentity, outputIdentity;
	if (!identity(input, &inputIdentity) || !identity(output, &outputIdentity))
		goto cleanup;
	length = GetModuleFileNameA(NULL, image, sizeof(image));
	if (!length || length >= sizeof(image))
		goto cleanup;
	int formatted = snprintf(command, sizeof(command), "\"%s\" child \"%s\" %08lx %08lx %08lx %08lx %08lx %08lx", image,
							 reportPath, (unsigned long)inputIdentity.volume, (unsigned long)inputIdentity.high,
							 (unsigned long)inputIdentity.low, (unsigned long)outputIdentity.volume,
							 (unsigned long)outputIdentity.high, (unsigned long)outputIdentity.low);
	if (formatted <= 0 || (size_t)formatted >= sizeof(command))
		goto cleanup;
	HANDLE replacements[3] = {input, output, output};
	for (; standardsChanged < 3; ++standardsChanged)
		if (!SetStdHandle(standards[standardsChanged], replacements[standardsChanged]))
			goto cleanup;
	SetLastError(0x4321);
	created = CreateProcessA(image, command, NULL, NULL, inherit, 0, NULL, NULL, &startup, &child);
	creationError = GetLastError();
	if (!created || WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0 ||
		!GetExitCodeProcess(child.hProcess, &exitCode) || exitCode || !readReport(reportPath, &report) ||
		!position(input, 0, FILE_CURRENT, &inputPosition) || !position(output, 0, FILE_CURRENT, &outputPosition))
		goto cleanup;
	const HANDLE parentIds[3] = {input, output, output};
	const BOOL expectedFlags[3] = {inputInherit, outputInherit, outputInherit};
	if (report.startupFlags & STARTF_USESTDHANDLES)
		goto cleanup;
	for (unsigned index = 0; index < 3; ++index) {
		const struct StandardObservation *item = &report.standard[index];
		const BOOL usable = !inherit || expectedFlags[index];
		if (inherit && item->handle != (uint64_t)(uintptr_t)parentIds[index])
			goto cleanup;
		if (!inherit && (item->handle == (uint64_t)(uintptr_t)parentIds[index] || !item->handle))
			goto cleanup;
		if (usable) {
			if (item->type != FILE_TYPE_DISK || !item->flagsResult ||
				item->flags != (expectedFlags[index] ? HANDLE_FLAG_INHERIT : 0) || !item->identityMatches ||
				!item->operationResult || item->bytes != (index ? 3 : 2))
				goto cleanup;
		} else if (item->type != FILE_TYPE_UNKNOWN || item->typeError != ERROR_INVALID_HANDLE ||
				   item->identityMatches || item->operationResult) {
			goto cleanup;
		}
	}
	if (!inherit && (report.standard[0].handle == report.standard[1].handle ||
					 report.standard[1].handle == report.standard[2].handle ||
					 report.standard[0].handle == report.standard[2].handle))
		goto cleanup;
	if (inputPosition != (!inherit || inputInherit ? 5 : 3) || outputPosition != (!inherit || outputInherit ? 6 : 0))
		goto cleanup;
	if ((!inherit || inputInherit) && (report.deniedWrite || report.deniedWriteError != ERROR_ACCESS_DENIED ||
									   memcmp(report.standard[0].data, "34", 2)))
		goto cleanup;
	if (!inherit || outputInherit) {
		char actual[7] = {0};
		DWORD count = 0;
		if (!position(output, 0, FILE_BEGIN, NULL) || !ReadFile(output, actual, sizeof(actual), &count, NULL) ||
			count != 6 || memcmp(actual, "outerr", 6))
			goto cleanup;
	}
	result = 0;
cleanup:
	int restored = 1;
	while (standardsChanged) {
		--standardsChanged;
		if (!SetStdHandle(standards[standardsChanged], saved[standardsChanged]))
			restored = 0;
	}
	if (child.hProcess && WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0) {
		BOOL terminated = TerminateProcess(child.hProcess, 101);
		DWORD reaped = WaitForSingleObject(child.hProcess, 5000);
		fprintf(stderr, "cleanup terminated=%u wait=%lu\n", (unsigned)terminated, (unsigned long)reaped);
		if (!terminated || reaped != WAIT_OBJECT_0)
			result = 1;
	}
	printf("case input-inherit=%u output-inherit=%u inherit=%u created=%u error=%lu exit=%lu "
		   "parent-input=%llx parent-output=%llx input-position=%lld output-position=%lld\n",
		   (unsigned)inputInherit, (unsigned)outputInherit, (unsigned)inherit, (unsigned)created,
		   (unsigned long)creationError, (unsigned long)exitCode, (unsigned long long)(uintptr_t)input,
		   (unsigned long long)(uintptr_t)output, (long long)inputPosition, (long long)outputPosition);
	if (!result) {
		for (unsigned index = 0; index < 3; ++index) {
			const struct StandardObservation *item = &report.standard[index];
			printf(" std=%u id=%llx type=%lu error=%lu flags-ok=%lu flags=%lu error=%lu identity-ok=%lu "
				   "error=%lu matches=%lu operation=%lu error=%lu bytes=%lu data=%02x%02x%02x\n",
				   index, (unsigned long long)item->handle, (unsigned long)item->type, (unsigned long)item->typeError,
				   (unsigned long)item->flagsResult, (unsigned long)item->flags, (unsigned long)item->flagsError,
				   (unsigned long)item->identityResult, (unsigned long)item->identityError,
				   (unsigned long)item->identityMatches, (unsigned long)item->operationResult,
				   (unsigned long)item->operationError, (unsigned long)item->bytes, item->data[0], item->data[1],
				   item->data[2]);
		}
	}
	int threadClosed = closeOwned(&child.hThread);
	int processClosed = closeOwned(&child.hProcess);
	int inputClosed = closeOwned(&input);
	int outputClosed = closeOwned(&output);
	int extraClosed = closeOwned(&extra);
	int inputDeleted = !inputPath[0] || DeleteFileA(inputPath);
	int outputDeleted = !outputPath[0] || DeleteFileA(outputPath);
	int reportDeleted = !reportPath[0] || DeleteFileA(reportPath);
	return result || !restored || !threadClosed || !processClosed || !inputClosed || !outputClosed || !extraClosed ||
		   !inputDeleted || !outputDeleted || !reportDeleted;
}

int main(int argc, char **argv) {
	if (argc == 9 && !strcmp(argv[1], "child"))
		return childMain(argv);
	if (argc != 1)
		return 2;
	int result = 0;
	for (unsigned input = 0; input < 2; ++input)
		for (unsigned output = 0; output < 2; ++output)
			for (unsigned inherit = 0; inherit < 2; ++inherit)
				result |= runCase(input, output, inherit);
	return result;
}
