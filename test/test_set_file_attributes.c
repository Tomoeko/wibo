#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdio.h>

enum { kErrorSeed = 17185 };

static unsigned failures;

static void checkEqual(const char *label, DWORD expected, DWORD actual) {
	if (expected != actual) {
		++failures;
		fprintf(stderr, "attribute context %s: expected %lu, received %lu\n", label, (unsigned long)expected,
				(unsigned long)actual);
	}
}

static void checkSet(const char *label, const char *nameA, const WCHAR *nameW, DWORD requested,
				 DWORD expectedAttributes) {
	for (unsigned wide = 0; wide < 2; ++wide) {
		DWORD before = GetFileAttributesA(nameA);
		checkEqual("before-valid", FALSE, before == INVALID_FILE_ATTRIBUTES);
		SetLastError(kErrorSeed);
		BOOL changed = wide ? SetFileAttributesW(nameW, requested) : SetFileAttributesA(nameA, requested);
		DWORD error = GetLastError();
		checkEqual(label, TRUE, changed);
		checkEqual("successful-last-error", kErrorSeed, error);
		checkEqual(label, expectedAttributes, GetFileAttributesA(nameA));
		checkEqual("wide-query", expectedAttributes, GetFileAttributesW(nameW));
	}
}

static void checkFailure(const char *label, const char *nameA, const WCHAR *nameW, DWORD errorExpected) {
	SetLastError(kErrorSeed);
	BOOL changed = SetFileAttributesA(nameA, FILE_ATTRIBUTE_NORMAL);
	DWORD error = GetLastError();
	checkEqual(label, FALSE, changed);
	checkEqual(label, errorExpected, error);
	SetLastError(kErrorSeed);
	changed = SetFileAttributesW(nameW, FILE_ATTRIBUTE_NORMAL);
	error = GetLastError();
	checkEqual(label, FALSE, changed);
	checkEqual(label, errorExpected, error);
}

static int readFirstByte(const char *name) {
	HANDLE file = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return -1;
	char value = 0;
	DWORD count = 0;
	BOOL read = ReadFile(file, &value, 1, &count, NULL);
	CloseHandle(file);
	return read && count == 1 ? (unsigned char)value : -1;
}

static void checkReadOnlyReplacement(void) {
	HANDLE source = CreateFileA("replacement.bin", GENERIC_WRITE,
								FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_NEW,
								FILE_ATTRIBUTE_NORMAL, NULL);
	if (source == INVALID_HANDLE_VALUE) {
		++failures;
		return;
	}
	DWORD written = 0;
	BOOL prepared = WriteFile(source, "y", 1, &written, NULL);
	BOOL closed = CloseHandle(source);
	if (!prepared || written != 1 || !closed) {
		++failures;
		DeleteFileA("replacement.bin");
		return;
	}
	for (unsigned wide = 0; wide < 2; ++wide) {
		SetLastError(kErrorSeed);
		BOOL moved = wide ? MoveFileExW(L"replacement.bin", L"fixture.bin", MOVEFILE_REPLACE_EXISTING)
					  : MoveFileExA("replacement.bin", "fixture.bin", MOVEFILE_REPLACE_EXISTING);
		DWORD error = GetLastError();
		checkEqual("replace-readonly", FALSE, moved);
		checkEqual("replace-readonly-error", ERROR_ACCESS_DENIED, error);
		checkEqual("replacement-source-attributes", FILE_ATTRIBUTE_ARCHIVE,
				   GetFileAttributesA("replacement.bin"));
		checkEqual("replacement-destination-attributes", FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE,
				   GetFileAttributesA("fixture.bin"));
		checkEqual("replacement-source-content", 'y', (DWORD)readFirstByte("replacement.bin"));
		checkEqual("replacement-destination-content", 'x', (DWORD)readFirstByte("fixture.bin"));
	}
	if (!DeleteFileA("replacement.bin"))
		++failures;
}

static void checkReadOnlyAccess(void) {
	HANDLE existing = CreateFileA("fixture.bin", GENERIC_READ | GENERIC_WRITE,
								  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
								  FILE_ATTRIBUTE_NORMAL, NULL);
	if (existing == INVALID_HANDLE_VALUE) {
		++failures;
		return;
	}
	SetLastError(kErrorSeed);
	checkEqual("set-readonly", TRUE,
			   SetFileAttributesA("fixture.bin", FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE));
	checkEqual("set-readonly-error", kErrorSeed, GetLastError());
	checkEqual("readonly-query", FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE,
			   GetFileAttributesA("fixture.bin"));
	SetLastError(kErrorSeed);
	HANDLE newWrite = CreateFileA("fixture.bin", GENERIC_WRITE,
								 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
								 FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD error = GetLastError();
	checkEqual("new-write-open", FALSE, newWrite != INVALID_HANDLE_VALUE);
	checkEqual("new-write-error", ERROR_ACCESS_DENIED, error);
	if (newWrite != INVALID_HANDLE_VALUE)
		checkEqual("close-unexpected-write", TRUE, CloseHandle(newWrite));
	DWORD written = 0;
	SetLastError(kErrorSeed);
	checkEqual("preopened-write", TRUE, WriteFile(existing, "x", 1, &written, NULL));
	checkEqual("preopened-write-error", kErrorSeed, GetLastError());
	checkEqual("preopened-write-count", 1, written);
	SetLastError(kErrorSeed);
	checkEqual("delete-readonly", FALSE, DeleteFileA("fixture.bin"));
	checkEqual("delete-readonly-error", ERROR_ACCESS_DENIED, GetLastError());
	checkEqual("close-preopened", TRUE, CloseHandle(existing));
	checkReadOnlyReplacement();
	SetLastError(kErrorSeed);
	checkEqual("clear-readonly", TRUE, SetFileAttributesA("fixture.bin", FILE_ATTRIBUTE_NORMAL));
	checkEqual("clear-readonly-error", kErrorSeed, GetLastError());
	checkEqual("cleared-query", FILE_ATTRIBUTE_ARCHIVE, GetFileAttributesA("fixture.bin"));
}

int main(void) {
	char original[MAX_PATH] = {0}, temporary[MAX_PATH] = {0}, root[MAX_PATH] = {0};
	BOOL seedCreated = FALSE, rootCreated = FALSE, fileCreated = FALSE, directoryCreated = FALSE;
	DWORD length = GetCurrentDirectoryA(MAX_PATH, original);
	if (!length || length >= MAX_PATH)
		goto failedSetup;
	length = GetTempPathA(MAX_PATH, temporary);
	if (!length || length >= MAX_PATH || !GetTempFileNameA(temporary, "sfa", 0, root))
		goto failedSetup;
	seedCreated = TRUE;
	if (!DeleteFileA(root))
		goto failedSetup;
	seedCreated = FALSE;
	if (!CreateDirectoryA(root, NULL))
		goto failedSetup;
	rootCreated = TRUE;
	if (!SetCurrentDirectoryA(root))
		goto failedSetup;
	HANDLE file = CreateFileA("fixture.bin", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_NEW,
								 FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		goto failedSetup;
	fileCreated = TRUE;
	if (!CloseHandle(file) || !CreateDirectoryA("fixture-dir", NULL))
		goto failedSetup;
	directoryCreated = TRUE;

	checkEqual("initial-file", FILE_ATTRIBUTE_ARCHIVE, GetFileAttributesA("fixture.bin"));
	checkEqual("initial-directory", FILE_ATTRIBUTE_DIRECTORY, GetFileAttributesA("fixture-dir"));
	const struct {
		DWORD requested;
		DWORD expected;
		const char *label;
	} fileCases[] = {{FILE_ATTRIBUTE_ARCHIVE, FILE_ATTRIBUTE_ARCHIVE, "file-archive"},
					  {FILE_ATTRIBUTE_NORMAL, FILE_ATTRIBUTE_ARCHIVE, "file-normal"},
					  {FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE,
					   FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE, "file-readonly"},
					  {FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE,
					   FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE, "file-hidden"},
					  {FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_ARCHIVE, FILE_ATTRIBUTE_ARCHIVE,
					   "file-not-indexed"},
					  {FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_ARCHIVE,
					   FILE_ATTRIBUTE_ARCHIVE, "file-temporary-not-indexed"},
					  {FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_ARCHIVE,
					   FILE_ATTRIBUTE_ARCHIVE, "file-request-directory"},
					  {0, FILE_ATTRIBUTE_ARCHIVE, "file-zero"}};
	for (unsigned index = 0; index < sizeof(fileCases) / sizeof(fileCases[0]); ++index)
		checkSet(fileCases[index].label, "fixture.bin", L"fixture.bin", fileCases[index].requested,
				 fileCases[index].expected);
	checkReadOnlyAccess();
	const struct {
		DWORD requested;
		DWORD expected;
		const char *label;
	} directoryCases[] = {{FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
							FILE_ATTRIBUTE_DIRECTORY, "directory-not-indexed"},
						   {FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_HIDDEN,
							FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN, "directory-hidden-not-indexed"},
						   {FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY, FILE_ATTRIBUTE_DIRECTORY,
							"directory-readonly"},
						   {FILE_ATTRIBUTE_NORMAL, FILE_ATTRIBUTE_DIRECTORY, "directory-normal"}};
	for (unsigned index = 0; index < sizeof(directoryCases) / sizeof(directoryCases[0]); ++index)
		checkSet(directoryCases[index].label, "fixture-dir", L"fixture-dir", directoryCases[index].requested,
				 directoryCases[index].expected);
	checkFailure("missing-file", "missing.bin", L"missing.bin", ERROR_FILE_NOT_FOUND);
	checkFailure("missing-parent", "missing-dir\\fixture.bin", L"missing-dir\\fixture.bin", ERROR_PATH_NOT_FOUND);
	checkFailure("null-name", NULL, NULL, ERROR_PATH_NOT_FOUND);
	checkFailure("empty-name", "", L"", ERROR_PATH_NOT_FOUND);

failedSetup:
	if (rootCreated) {
		if (!SetCurrentDirectoryA(root))
			++failures;
		if (fileCreated && (!SetFileAttributesA("fixture.bin", FILE_ATTRIBUTE_NORMAL) ||
							!DeleteFileA("fixture.bin")))
			++failures;
		if (directoryCreated && (!SetFileAttributesA("fixture-dir", FILE_ATTRIBUTE_NORMAL) ||
								 !RemoveDirectoryA("fixture-dir")))
			++failures;
		if (!SetCurrentDirectoryA(original) || !RemoveDirectoryA(root))
			++failures;
	} else if (seedCreated && !DeleteFileA(root)) {
		++failures;
	}
	if (!rootCreated || !fileCreated || !directoryCreated)
		++failures;
	printf("set_file_attribute_contexts=%lu directory_contexts=%lu encodings=2 failures=%u\n",
		   (unsigned long)(sizeof(fileCases) / sizeof(fileCases[0])),
		   (unsigned long)(sizeof(directoryCases) / sizeof(directoryCases[0])), failures);
	return failures ? 1 : 0;
}
