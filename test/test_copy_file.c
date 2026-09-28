#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdio.h>
#include <string.h>

enum { kSeed = 17185, kSourceSize = 131072 };

static unsigned failures;

static void check(const char *label, DWORD expected, DWORD actual) {
	if (expected != actual) {
		++failures;
		fprintf(stderr, "%s: expected %lu, received %lu\n", label, (unsigned long)expected, (unsigned long)actual);
	}
}

static BOOL createSource(void) {
	HANDLE file = CreateFileA("source.bin", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	char bytes[8192];
	memset(bytes, 'S', sizeof bytes);
	BOOL okay = TRUE;
	for (unsigned index = 0; index < kSourceSize / sizeof bytes; ++index) {
		DWORD written = 0;
		if (!WriteFile(file, bytes, sizeof bytes, &written, NULL) || written != sizeof bytes) {
			okay = FALSE;
			break;
		}
	}
	return CloseHandle(file) && okay;
}

static BOOL createDestination(const char *name, char value) {
	HANDLE file = CreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	DWORD written = 0;
	BOOL okay = WriteFile(file, &value, 1, &written, NULL);
	return CloseHandle(file) && okay && written == 1;
}

static int firstByte(const char *name) {
	HANDLE file = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return -1;
	char value = 0;
	DWORD read = 0;
	BOOL okay = ReadFile(file, &value, 1, &read, NULL);
	CloseHandle(file);
	return okay && read == 1 ? (unsigned char)value : -1;
}

static DWORD sizeOf(const char *name) {
	HANDLE file = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return INVALID_FILE_SIZE;
	DWORD size = GetFileSize(file, NULL);
	CloseHandle(file);
	return size;
}

struct ProgressState {
	unsigned calls;
	DWORD firstReason;
	DWORD action;
	BOOL invalid;
};

static DWORD CALLBACK progress(LARGE_INTEGER total, LARGE_INTEGER transferred, LARGE_INTEGER streamSize,
							   LARGE_INTEGER streamTransferred, DWORD streamNumber, DWORD reason, HANDLE source,
							   HANDLE destination, LPVOID data) {
	struct ProgressState *state = (struct ProgressState *)data;
	if (state->calls++ == 0)
		state->firstReason = reason;
	if (total.QuadPart != kSourceSize || streamSize.QuadPart != kSourceSize || transferred.QuadPart < 0 ||
		transferred.QuadPart > total.QuadPart || streamTransferred.QuadPart != transferred.QuadPart ||
		streamNumber != 1 || source == INVALID_HANDLE_VALUE || destination == INVALID_HANDLE_VALUE)
		state->invalid = TRUE;
	return state->action;
}

static void checkAbort(const char *name, DWORD action, BOOL shouldExist) {
	struct ProgressState state = {0, 0, action, FALSE};
	SetLastError(kSeed);
	BOOL result = CopyFileExA("source.bin", name, progress, &state, NULL, 0);
	DWORD error = GetLastError();
	check("callback-result", FALSE, result);
	check("callback-error", ERROR_REQUEST_ABORTED, error);
	check("callback-count", TRUE, state.calls >= 1);
	check("callback-first-reason", CALLBACK_STREAM_SWITCH, state.firstReason);
	check("callback-parameters", FALSE, state.invalid);
	check("callback-destination-exists", shouldExist, GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES);
	if (shouldExist)
		check("callback-destination-delete", TRUE, DeleteFileA(name));
}

int main(int argc, char **argv) {
	const BOOL nativeBaseline = argc > 1 && strcmp(argv[1], "--native-baseline") == 0;
	char original[MAX_PATH] = {0}, temporary[MAX_PATH] = {0}, directory[MAX_PATH] = {0};
	BOOL seedCreated = FALSE, directoryCreated = FALSE, sourceCreated = FALSE;
	DWORD length = GetCurrentDirectoryA(MAX_PATH, original);
	if (!length || length >= MAX_PATH)
		goto cleanup;
	length = GetTempPathA(MAX_PATH, temporary);
	if (!length || length >= MAX_PATH || !GetTempFileNameA(temporary, "cpy", 0, directory))
		goto cleanup;
	seedCreated = TRUE;
	if (!DeleteFileA(directory))
		goto cleanup;
	seedCreated = FALSE;
	if (!CreateDirectoryA(directory, NULL))
		goto cleanup;
	directoryCreated = TRUE;
	if (!SetCurrentDirectoryA(directory) || !createSource())
		goto cleanup;
	sourceCreated = TRUE;

	if (argc > 1 && strcmp(argv[1], "--unsupported") == 0) {
		SetLastError(kSeed);
		check("unsupported-flag", FALSE,
			  CopyFileExW(L"source.bin", L"unsupported.bin", NULL, NULL, NULL, COPY_FILE_RESTARTABLE));
		check("unsupported-error", ERROR_NOT_SUPPORTED, GetLastError());
		check("unsupported-destination", INVALID_FILE_ATTRIBUTES, GetFileAttributesA("unsupported.bin"));
		goto cleanup;
	}

	SetLastError(kSeed);
	BOOL copiedA = CopyFileA("source.bin", "simple-a.bin", TRUE);
	DWORD copiedAError = GetLastError();
	printf("copy_a_success_error=%lu\n", (unsigned long)copiedAError);
	check("copy-a", TRUE, copiedA);
	check("copy-a-size", kSourceSize, sizeOf("simple-a.bin"));
	check("copy-a-data", 'S', (DWORD)firstByte("simple-a.bin"));
	check("copy-w", TRUE, CopyFileW(L"source.bin", L"simple-w.bin", TRUE));
	check("copy-w-size", kSourceSize, sizeOf("simple-w.bin"));
	SetLastError(kSeed);
	check("copy-fail-if-exists", FALSE,
		  CopyFileExA("source.bin", "simple-a.bin", NULL, NULL, NULL, COPY_FILE_FAIL_IF_EXISTS));
	check("copy-fail-if-exists-error", ERROR_FILE_EXISTS, GetLastError());
	check("copy-fail-if-exists-size", kSourceSize, sizeOf("simple-a.bin"));
	if (!createDestination("overwrite.bin", 'D'))
		goto cleanup;
	SetLastError(kSeed);
	BOOL copiedExA = CopyFileExA("source.bin", "overwrite.bin", NULL, NULL, NULL, 0);
	DWORD copiedExAError = GetLastError();
	printf("copy_ex_a_overwrite_success_error=%lu\n", (unsigned long)copiedExAError);
	check("copy-ex-a-overwrite", TRUE, copiedExA);
	check("copy-ex-a-overwrite-size", kSourceSize, sizeOf("overwrite.bin"));
	check("copy-ex-w-overwrite", TRUE, CopyFileExW(L"source.bin", L"overwrite.bin", NULL, NULL, NULL, 0));
	check("copy-ex-w-overwrite-size", kSourceSize, sizeOf("overwrite.bin"));
	check("copy-symlink-flag-a", TRUE,
		  CopyFileExA("source.bin", "link-option-a.bin", NULL, NULL, NULL, COPY_FILE_COPY_SYMLINK));
	check("copy-symlink-flag-a-size", kSourceSize, sizeOf("link-option-a.bin"));
	check("copy-symlink-flag-a-content", 'S', (DWORD)firstByte("link-option-a.bin"));
	check("copy-symlink-flag-w", TRUE,
		  CopyFileExW(L"source.bin", L"link-option-w.bin", NULL, NULL, NULL, COPY_FILE_COPY_SYMLINK));
	check("copy-symlink-flag-w-size", kSourceSize, sizeOf("link-option-w.bin"));
	SetLastError(kSeed);
	check("copy-symlink-flag-existing", FALSE,
		  CopyFileExW(L"source.bin", L"link-option-a.bin", NULL, NULL, NULL,
					  COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_COPY_SYMLINK));
	check("copy-symlink-flag-existing-error", ERROR_FILE_EXISTS, GetLastError());
	check("copy-symlink-flag-existing-size", kSourceSize, sizeOf("link-option-a.bin"));
	if (!createDestination("readonly.bin", 'R'))
		goto cleanup;
	check("set-readonly", TRUE, SetFileAttributesA("readonly.bin", FILE_ATTRIBUTE_READONLY));
	SetLastError(kSeed);
	check("copy-readonly-result", FALSE, CopyFileExW(L"source.bin", L"readonly.bin", NULL, NULL, NULL, 0));
	check("copy-readonly-error", ERROR_ACCESS_DENIED, GetLastError());
	check("copy-readonly-content", 'R', (DWORD)firstByte("readonly.bin"));
	check("clear-readonly", TRUE, SetFileAttributesA("readonly.bin", FILE_ATTRIBUTE_NORMAL));
	if (!nativeBaseline) {
		if (!createDestination("hidden.bin", 'H'))
			goto cleanup;
		check("set-hidden", TRUE, SetFileAttributesA("hidden.bin", FILE_ATTRIBUTE_HIDDEN));
		SetLastError(kSeed);
		check("copy-hidden-result", FALSE, CopyFileExA("source.bin", "hidden.bin", NULL, NULL, NULL, 0));
		check("copy-hidden-error", ERROR_ACCESS_DENIED, GetLastError());
		check("copy-hidden-content", 'H', (DWORD)firstByte("hidden.bin"));
		check("clear-hidden", TRUE, SetFileAttributesA("hidden.bin", FILE_ATTRIBUTE_NORMAL));

		struct ProgressState state = {0, 0, PROGRESS_CONTINUE, FALSE};
		check("callback-continue-copy", TRUE, CopyFileExA("source.bin", "progress.bin", progress, &state, NULL, 0));
		check("callback-continue-count", TRUE, state.calls >= 1);
		check("callback-continue-first-reason", CALLBACK_STREAM_SWITCH, state.firstReason);
		check("callback-continue-parameters", FALSE, state.invalid);
		check("callback-continue-size", kSourceSize, sizeOf("progress.bin"));
		state = (struct ProgressState){0, 0, PROGRESS_QUIET, FALSE};
		check("callback-quiet-copy", TRUE, CopyFileExW(L"source.bin", L"quiet.bin", progress, &state, NULL, 0));
		check("callback-quiet-count", 1, state.calls);
		check("callback-quiet-size", kSourceSize, sizeOf("quiet.bin"));
		checkAbort("cancel.bin", PROGRESS_CANCEL, FALSE);
		checkAbort("stop.bin", PROGRESS_STOP, TRUE);
		BOOL canceled = TRUE;
		SetLastError(kSeed);
		check("cancel-pointer-result", FALSE, CopyFileExA("source.bin", "pointer.bin", NULL, NULL, &canceled, 0));
		check("cancel-pointer-error", ERROR_REQUEST_ABORTED, GetLastError());
		check("cancel-pointer-destination", INVALID_FILE_ATTRIBUTES, GetFileAttributesA("pointer.bin"));
	}
	check("source-preserved", kSourceSize, sizeOf("source.bin"));

cleanup:
	if (directoryCreated) {
		SetCurrentDirectoryA(directory);
		const char *names[] = {"link-option-a.bin", "link-option-w.bin", "source.bin",	 "simple-a.bin",
							   "simple-w.bin",		"overwrite.bin",	 "readonly.bin", "hidden.bin",
							   "progress.bin",		"quiet.bin",		 "cancel.bin",	 "stop.bin",
							   "pointer.bin",		"unsupported.bin"};
		for (unsigned index = 0; index < sizeof names / sizeof names[0]; ++index) {
			if (GetFileAttributesA(names[index]) != INVALID_FILE_ATTRIBUTES) {
				SetFileAttributesA(names[index], FILE_ATTRIBUTE_NORMAL);
				if (!DeleteFileA(names[index]))
					++failures;
			}
		}
		if (!SetCurrentDirectoryA(original) || !RemoveDirectoryA(directory))
			++failures;
	} else if (seedCreated && !DeleteFileA(directory)) {
		++failures;
	}
	if (!directoryCreated || !sourceCreated)
		++failures;
	printf("copy_file_contexts=16 encoding_variants=2 failures=%u\n", failures);
	return failures ? 1 : 0;
}
