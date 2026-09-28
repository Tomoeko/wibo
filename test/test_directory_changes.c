#include "test_assert.h"
#include <windows.h>

static DWORD caller, callbackCount, callbackError, callbackBytes;
static OVERLAPPED *callbackOperation;
static void CALLBACK completed(DWORD error, DWORD bytes, OVERLAPPED *operation) {
	TEST_CHECK_EQ(caller, GetCurrentThreadId());
	callbackError = error;
	callbackBytes = bytes;
	callbackOperation = operation;
	++callbackCount;
}
static HANDLE threadDirectory;
static OVERLAPPED threadOperation;
static DWORD threadBuffer[1024];
static DWORD WINAPI issueAndExit(void *unused) {
	(void)unused;
	TEST_CHECK(ReadDirectoryChangesW(threadDirectory, threadBuffer, sizeof(threadBuffer), FALSE,
									 FILE_NOTIFY_CHANGE_FILE_NAME, NULL, &threadOperation, NULL));
	return 0;
}
static DWORD WINAPI delayedCreate(void *path) {
	Sleep(100);
	HANDLE file =
		CreateFileA((const char *)path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_NEW, 0, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	TEST_CHECK(CloseHandle(file));
	return 0;
}
static HANDLE openDirectory(const char *path) {
	HANDLE handle = CreateFileA(path, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
								OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
	TEST_CHECK(handle != INVALID_HANDLE_VALUE);
	return handle;
}
static void createFile(const char *path) {
	HANDLE handle = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_NEW, 0, NULL);
	TEST_CHECK(handle != INVALID_HANDLE_VALUE);
	TEST_CHECK(CloseHandle(handle));
}
static void checkName(BYTE *buffer, DWORD bytes, DWORD action, const WCHAR *name) {
	TEST_CHECK(bytes >= 12);
	FILE_NOTIFY_INFORMATION *info = (FILE_NOTIFY_INFORMATION *)buffer;
	TEST_CHECK_EQ(action, info->Action);
	TEST_CHECK_EQ(wcslen(name) * sizeof(WCHAR), info->FileNameLength);
	TEST_CHECK(memcmp(info->FileName, name, info->FileNameLength) == 0);
}
int main(void) {
	char root[MAX_PATH], child[MAX_PATH], nested[MAX_PATH], nestedFile[MAX_PATH];
	GetTempPathA(sizeof(root), root);
	char suffix[60];
	snprintf(suffix, sizeof(suffix), "watch-%lu-%lu", GetCurrentProcessId(), GetTickCount());
	strcat(root, suffix);
	snprintf(child, sizeof(child), "%s\\created.bin", root);
	snprintf(nested, sizeof(nested), "%s\\nested", root);
	snprintf(nestedFile, sizeof(nestedFile), "%s\\deep.bin", nested);
	TEST_CHECK(CreateDirectoryA(root, NULL));
	TEST_CHECK(CreateDirectoryA(nested, NULL));
	HANDLE directory = openDirectory(root);
	HANDLE event = CreateEventA(NULL, TRUE, TRUE, NULL);
	TEST_CHECK(event != NULL);
	DWORD buffer[1024];
	OVERLAPPED operation = {0};
	operation.hEvent = event;
	DWORD bytes = 0;
	TEST_CHECK(ReadDirectoryChangesW(directory, buffer, sizeof(buffer), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, &bytes,
									 &operation, NULL));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(event, 0));
	createFile(nestedFile);
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(event, 200));
	createFile(child);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(event, 5000));
	TEST_CHECK(GetOverlappedResult(directory, &operation, &bytes, FALSE));
	checkName((BYTE *)buffer, bytes, FILE_ACTION_ADDED, L"created.bin");
	// Changes between reads remain buffered on the open directory.
	TEST_CHECK(DeleteFileA(child));
	Sleep(250);
	memset(&operation, 0, sizeof(operation));
	operation.hEvent = event;
	TEST_CHECK(ReadDirectoryChangesW(directory, buffer, sizeof(buffer), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, NULL,
									 &operation, NULL));
	TEST_CHECK(GetOverlappedResult(directory, &operation, &bytes, TRUE));
	if (bytes)
		checkName((BYTE *)buffer, bytes, FILE_ACTION_REMOVED, L"created.bin");
	TEST_CHECK_EQ(INVALID_FILE_ATTRIBUTES, GetFileAttributesA(child));
	memset(&operation, 0, sizeof(operation));
	operation.hEvent = event;
	TEST_CHECK(ReadDirectoryChangesW(directory, buffer, sizeof(buffer), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, NULL,
									 &operation, NULL));
	TEST_CHECK(CancelIo(directory));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(event, 5000));
	TEST_CHECK(!GetOverlappedResult(directory, &operation, &bytes, FALSE));
	TEST_CHECK_EQ(ERROR_OPERATION_ABORTED, GetLastError());
	TEST_CHECK(CancelIo(directory));
	TEST_CHECK(CloseHandle(directory));

	snprintf(child, sizeof(child), "%s\\callback.bin", root);
	// Completion callbacks run only in an alertable wait on the issuing thread.
	directory = openDirectory(root);
	caller = GetCurrentThreadId();
	memset(&operation, 0, sizeof(operation));
	operation.hEvent = event;
	ResetEvent(event);
	TEST_CHECK(ReadDirectoryChangesW(directory, buffer, sizeof(buffer), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME, NULL,
									 &operation, completed));
	createFile(child);
	Sleep(250);
	TEST_CHECK_EQ(0, callbackCount);
	TEST_CHECK_EQ(WAIT_IO_COMPLETION, SleepEx(5000, TRUE));
	TEST_CHECK_EQ(1, callbackCount);
	TEST_CHECK_EQ(0, callbackError);
	TEST_CHECK(callbackOperation == &operation);
	if (callbackBytes)
		checkName((BYTE *)buffer, callbackBytes, FILE_ACTION_ADDED, L"callback.bin");
	TEST_CHECK(GetFileAttributesA(child) != INVALID_FILE_ATTRIBUTES);
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(event, 0));
	TEST_CHECK(ReadDirectoryChangesW(directory, buffer, sizeof(buffer), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME, NULL,
									 &operation, completed));
	TEST_CHECK(CancelIoEx(directory, &operation));
	TEST_CHECK(CloseHandle(directory));
	TEST_CHECK_EQ(WAIT_IO_COMPLETION, SleepEx(5000, TRUE));
	TEST_CHECK_EQ(2, callbackCount);
	TEST_CHECK_EQ(ERROR_OPERATION_ABORTED, callbackError);

	TEST_CHECK(DeleteFileA(child));
	snprintf(child, sizeof(child), "%s\\port.bin", root);
	// Completion ports receive the directory's association key and operation.
	directory = openDirectory(root);
	HANDLE port = CreateIoCompletionPort(directory, NULL, 0x7135, 1);
	TEST_CHECK(port != NULL);
	memset(&operation, 0, sizeof(operation));
	TEST_CHECK(ReadDirectoryChangesW(directory, buffer, sizeof(buffer), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, NULL,
									 &operation, NULL));
	createFile(child);
	ULONG_PTR key = 0;
	OVERLAPPED *received = NULL;
	TEST_CHECK(GetQueuedCompletionStatus(port, &bytes, &key, &received, 5000));
	TEST_CHECK_EQ(0x7135, key);
	TEST_CHECK(received == &operation);
	if (bytes)
		checkName((BYTE *)buffer, bytes, FILE_ACTION_ADDED, L"port.bin");
	TEST_CHECK(GetFileAttributesA(child) != INVALID_FILE_ATTRIBUTES);
	threadDirectory = directory;
	memset(&threadOperation, 0, sizeof(threadOperation));
	HANDLE thread = CreateThread(NULL, 0, issueAndExit, NULL, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	TEST_CHECK(!GetQueuedCompletionStatus(port, &bytes, &key, &received, 100));
	TEST_CHECK_EQ(WAIT_TIMEOUT, GetLastError());
	TEST_CHECK(DeleteFileA(child));
	snprintf(child, sizeof(child), "%s\\thread.bin", root);
	createFile(child);
	TEST_CHECK(GetQueuedCompletionStatus(port, &bytes, &key, &received, 5000));
	TEST_CHECK(received == &threadOperation);
	TEST_CHECK_EQ(0x7135, key);
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK(CloseHandle(directory));
	TEST_CHECK(CloseHandle(port));
	TEST_CHECK(DeleteFileA(child));
	// Closing the final handle retires outstanding callback requests.
	directory = openDirectory(root);
	memset(&operation, 0, sizeof(operation));
	TEST_CHECK(ReadDirectoryChangesW(directory, buffer, sizeof(buffer), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, NULL,
									 &operation, completed));
	TEST_CHECK(CloseHandle(directory));
	TEST_CHECK_EQ(WAIT_IO_COMPLETION, SleepEx(5000, TRUE));
	TEST_CHECK_EQ(3, callbackCount);
	TEST_CHECK_EQ(ERROR_HANDLES_CLOSED, callbackError);

	// A synchronous directory read blocks until a writer changes its contents.
	snprintf(child, sizeof(child), "%s\\synchronous.bin", root);
	directory = CreateFileA(root, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	TEST_CHECK(directory != INVALID_HANDLE_VALUE);
	thread = CreateThread(NULL, 0, delayedCreate, child, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK(ReadDirectoryChangesW(directory, buffer, sizeof(buffer), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, &bytes,
									 NULL, NULL));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	if (bytes)
		checkName((BYTE *)buffer, bytes, FILE_ACTION_ADDED, L"synchronous.bin");
	TEST_CHECK(GetFileAttributesA(child) != INVALID_FILE_ATTRIBUTES);
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK(CloseHandle(directory));
	TEST_CHECK(DeleteFileA(child));

	// A buffer too small for one record produces an explicit rescan indication.
	snprintf(child, sizeof(child), "%s\\overflow.bin", root);
	directory = openDirectory(root);
	memset(&operation, 0, sizeof(operation));
	operation.hEvent = event;
	buffer[0] = 0x7135;
	TEST_CHECK(
		ReadDirectoryChangesW(directory, buffer, 4, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, NULL, &operation, NULL));
	createFile(child);
	if (!GetOverlappedResult(directory, &operation, &bytes, TRUE))
		TEST_CHECK_EQ(ERROR_NOTIFY_ENUM_DIR, GetLastError());
	TEST_CHECK_EQ(0, bytes);
	TEST_CHECK_EQ(0x7135, buffer[0]);
	TEST_CHECK(CloseHandle(directory));
	TEST_CHECK(DeleteFileA(child));
	TEST_CHECK(CloseHandle(event));
	TEST_CHECK(DeleteFileA(nestedFile));
	TEST_CHECK(RemoveDirectoryA(nested));
	TEST_CHECK(RemoveDirectoryA(root));
	return 0;
}
