#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

static const DWORD seed = 0x4321;
static const DWORD shareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
static const char payload[] = "sharing-fixture";

static void attempt(const char *name, const char *path, const char *other, DWORD firstAccess, DWORD firstShare,
					DWORD secondAccess, DWORD secondShare, BOOL accepted) {
	HANDLE first = CreateFileA(path, firstAccess, firstShare, NULL, OPEN_EXISTING, 0, NULL);
	TEST_CHECK(first != INVALID_HANDLE_VALUE);
	SetLastError(seed);
	HANDLE second = CreateFileA(other, secondAccess, secondShare, NULL, OPEN_EXISTING, 0, NULL);
	DWORD error = GetLastError();
	printf("case=%s accepted=%d error=%lu\n", name, second != INVALID_HANDLE_VALUE, (unsigned long)error);
	TEST_CHECK_EQ(accepted, second != INVALID_HANDLE_VALUE);
	if (!accepted)
		TEST_CHECK_EQ(ERROR_SHARING_VIOLATION, error);
	if (second != INVALID_HANDLE_VALUE)
		TEST_CHECK(CloseHandle(second));
	TEST_CHECK(CloseHandle(first));
}

static void truncate_attempt(const char *path, DWORD disposition) {
	HANDLE first = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	TEST_CHECK(first != INVALID_HANDLE_VALUE);
	SetLastError(seed);
	HANDLE second = CreateFileA(path, GENERIC_WRITE, shareAll, NULL, disposition, 0, NULL);
	DWORD error = GetLastError();
	if (second != INVALID_HANDLE_VALUE)
		TEST_CHECK(CloseHandle(second));
	TEST_CHECK(second == INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(ERROR_SHARING_VIOLATION, error);
	DWORD length = GetFileSize(first, NULL), read = 0;
	char bytes[sizeof(payload)] = {0};
	TEST_CHECK(ReadFile(first, bytes, sizeof(bytes), &read, NULL));
	printf("truncate=%lu accepted=%d error=%lu length=%lu unchanged=%d\n", (unsigned long)disposition,
		   second != INVALID_HANDLE_VALUE, (unsigned long)error, (unsigned long)length,
		   read == sizeof(payload) && memcmp(bytes, payload, sizeof(payload)) == 0);
	TEST_CHECK_EQ(sizeof(payload), length);
	TEST_CHECK_EQ(sizeof(payload), read);
	TEST_CHECK(memcmp(bytes, payload, sizeof(payload)) == 0);
	TEST_CHECK(CloseHandle(first));
}

struct Worker {
	const char *path;
	HANDLE start, done, release;
	BOOL accepted;
	DWORD error;
	DWORD failure;
};

static DWORD WINAPI concurrent_open(void *parameter) {
	struct Worker *state = parameter;
	if (WaitForSingleObject(state->start, 5000) != WAIT_OBJECT_0) {
		state->failure = 1;
		SetEvent(state->done);
		return 1;
	}
	SetLastError(seed);
	HANDLE file = CreateFileA(state->path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	state->accepted = file != INVALID_HANDLE_VALUE;
	state->error = GetLastError();
	if (!SetEvent(state->done))
		state->failure = 2;
	if (WaitForSingleObject(state->release, 5000) != WAIT_OBJECT_0)
		state->failure = 3;
	if (state->accepted && !CloseHandle(file))
		state->failure = 4;
	return state->failure;
}

static void concurrent_attempt(const char *path) {
	HANDLE start = CreateEventA(NULL, TRUE, FALSE, NULL);
	HANDLE release = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(start && release);
	struct Worker workers[2] = {{0}, {0}};
	HANDLE threads[2] = {NULL, NULL};
	for (unsigned i = 0; i < 2; ++i) {
		workers[i].path = path;
		workers[i].start = start;
		workers[i].release = release;
		workers[i].done = CreateEventA(NULL, TRUE, FALSE, NULL);
		TEST_CHECK(workers[i].done != NULL);
		threads[i] = CreateThread(NULL, 0, concurrent_open, &workers[i], 0, NULL);
		TEST_CHECK(threads[i] != NULL);
	}
	TEST_CHECK(SetEvent(start));
	DWORD completed[2];
	for (unsigned i = 0; i < 2; ++i)
		completed[i] = WaitForSingleObject(workers[i].done, 5000);
	TEST_CHECK(SetEvent(release));
	DWORD joined[2];
	for (unsigned i = 0; i < 2; ++i)
		joined[i] = WaitForSingleObject(threads[i], 5000);
	for (unsigned i = 0; i < 2; ++i) {
		TEST_CHECK_EQ(WAIT_OBJECT_0, joined[i]);
		TEST_CHECK_EQ(WAIT_OBJECT_0, completed[i]);
		TEST_CHECK_EQ(0, workers[i].failure);
		if (!workers[i].accepted)
			TEST_CHECK_EQ(ERROR_SHARING_VIOLATION, workers[i].error);
		printf("concurrent=%u accepted=%d error=%lu\n", i, workers[i].accepted, (unsigned long)workers[i].error);
		TEST_CHECK(CloseHandle(threads[i]));
		TEST_CHECK(CloseHandle(workers[i].done));
	}
	TEST_CHECK_EQ(1, workers[0].accepted + workers[1].accepted);
	TEST_CHECK(CloseHandle(start));
	TEST_CHECK(CloseHandle(release));
}

int main(void) {
	char directory[MAX_PATH], path[MAX_PATH], alias[MAX_PATH];
	DWORD length = GetTempPathA(MAX_PATH, directory);
	TEST_CHECK(length && length < MAX_PATH);
	TEST_CHECK(GetTempFileNameA(directory, "wsh", 0, path));
	HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, shareAll, NULL, OPEN_EXISTING, 0, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	DWORD written = 0;
	TEST_CHECK(WriteFile(file, payload, sizeof(payload), &written, NULL));
	TEST_CHECK_EQ(sizeof(payload), written);
	TEST_CHECK(CloseHandle(file));

	static const struct {
		const char *name;
		DWORD access1, share1, access2, share2;
		BOOL accepted;
	} cases[] = {
		{"exclusive-read", GENERIC_READ, 0, GENERIC_READ, 7, FALSE},
		{"shared-read", GENERIC_READ, 1, GENERIC_READ, 1, TRUE},
		{"old-disallows-write", GENERIC_READ, 1, GENERIC_WRITE, 3, FALSE},
		{"old-disallows-read", GENERIC_WRITE, 2, GENERIC_READ, 3, FALSE},
		{"new-disallows-read", GENERIC_READ, 7, GENERIC_READ, 0, FALSE},
		{"shared-write", GENERIC_WRITE, 3, GENERIC_WRITE, 3, TRUE},
		{"delete-exclusive", DELETE, 0, GENERIC_READ, 7, FALSE},
		{"delete-shared", GENERIC_READ, 7, DELETE, 7, TRUE},
		{"old-disallows-delete", GENERIC_READ, 3, DELETE, 7, FALSE},
		{"new-disallows-delete", DELETE, 7, GENERIC_READ, 3, FALSE},
		{"execute-exclusive", FILE_EXECUTE, 0, FILE_READ_DATA, 7, FALSE},
		{"old-attributes-only", FILE_READ_ATTRIBUTES, 0, GENERIC_READ, 7, TRUE},
		{"new-attributes-only", GENERIC_READ, 0, FILE_READ_ATTRIBUTES, 0, TRUE},
		{"old-ea-only", FILE_READ_EA, 0, GENERIC_READ, 7, TRUE},
		{"old-write-attributes", FILE_WRITE_ATTRIBUTES, 0, GENERIC_WRITE, 7, TRUE},
		{"old-zero-access", 0, 0, GENERIC_READ, 0, TRUE},
		{"new-zero-access", GENERIC_READ, 0, 0, 0, TRUE},
		{"new-attributes-after-delete", DELETE, 7, FILE_READ_ATTRIBUTES, 0, TRUE},
		{"append-exclusive", FILE_APPEND_DATA, 1, FILE_WRITE_DATA, 7, FALSE},
		{"reverse-original-write", GENERIC_WRITE, 1, GENERIC_READ, 1, FALSE},
	};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
		attempt(cases[i].name, path, path, cases[i].access1, cases[i].share1, cases[i].access2, cases[i].share2,
				cases[i].accepted);

	const char *separator = strrchr(path, '\\');
	TEST_CHECK(separator != NULL);
	size_t prefix = (size_t)(separator - path) + 1, suffix = strlen(separator + 1);
	TEST_CHECK(prefix + suffix + 3 < MAX_PATH);
	memcpy(alias, path, prefix);
	memcpy(alias + prefix, ".\\", 2);
	memcpy(alias + prefix + 2, separator + 1, suffix + 1);
	attempt("dot-alias", path, alias, GENERIC_READ, 0, GENERIC_READ, shareAll, FALSE);

	file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	HANDLE duplicate = NULL;
	TEST_CHECK(
		DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &duplicate, FILE_WRITE_ATTRIBUTES, FALSE, 0));
	TEST_CHECK(CloseHandle(file));
	SetLastError(seed);
	HANDLE reader = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	DWORD error = GetLastError();
	printf("duplicate-reduced accepted=%d error=%lu\n", reader != INVALID_HANDLE_VALUE, (unsigned long)error);
	TEST_CHECK(reader == INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(ERROR_SHARING_VIOLATION, error);
	if (reader != INVALID_HANDLE_VALUE)
		TEST_CHECK(CloseHandle(reader));
	TEST_CHECK(CloseHandle(duplicate));
	SetLastError(seed);
	reader = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	error = GetLastError();
	printf("after-last-alias accepted=%d error=%lu\n", reader != INVALID_HANDLE_VALUE, (unsigned long)error);
	TEST_CHECK(reader != INVALID_HANDLE_VALUE);
	TEST_CHECK(CloseHandle(reader));

	truncate_attempt(path, CREATE_ALWAYS);
	truncate_attempt(path, TRUNCATE_EXISTING);
	concurrent_attempt(path);
	TEST_CHECK(DeleteFileA(path));
	return 0;
}
