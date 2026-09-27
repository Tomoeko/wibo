#include "test_assert.h"
#include <stdio.h>
#include <windows.h>

static char stateFile[80];

static void writeState(const char *state) {
	char temporary[96];
	snprintf(temporary, sizeof(temporary), "%s.new", stateFile);
	HANDLE file = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	DWORD written = 0;
	TEST_CHECK(WriteFile(file, state, (DWORD)strlen(state), &written, NULL));
	TEST_CHECK_EQ(strlen(state), written);
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK(MoveFileExA(temporary, stateFile, MOVEFILE_REPLACE_EXISTING));
}

static BOOL query(HANDLE handle) {
	BOOL state = 9;
	SetLastError(0x20001234);
	TEST_CHECK(QueryMemoryResourceNotification(handle, &state));
	TEST_CHECK_EQ(0x20001234, GetLastError());
	TEST_CHECK(state == FALSE || state == TRUE);
	return state;
}

int main(int argc, char **argv) {
	const BOOL synthetic = getenv("WIBO_FIXTURE_PROVIDER") != NULL;
	if (synthetic) {
		snprintf(stateFile, sizeof(stateFile), "memory-resource-state-%lu.tmp", GetCurrentProcessId());
		writeState("0,0");
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_MEMORY_RESOURCE_STATE_FILE", stateFile));
	}
	TEST_CHECK(CreateMemoryResourceNotification((MEMORY_RESOURCE_NOTIFICATION_TYPE)2) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	BOOL unchanged = 7;
	TEST_CHECK(!QueryMemoryResourceNotification(NULL, &unchanged));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK_EQ(7, unchanged);
	if (argc > 1 && strcmp(argv[1], "unavailable") == 0) {
		TEST_CHECK(CreateMemoryResourceNotification(LowMemoryResourceNotification) == NULL);
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		return 0;
	}
	if (synthetic && argc > 1) {
		writeState(argv[1]);
		TEST_CHECK(CreateMemoryResourceNotification(LowMemoryResourceNotification) == NULL);
		TEST_CHECK_EQ(strcmp(argv[1], "failed") == 0 ? ERROR_ACCESS_DENIED : ERROR_INVALID_DATA, GetLastError());
		TEST_CHECK(DeleteFileA(stateFile));
		return 0;
	}
	HANDLE low = CreateMemoryResourceNotification(LowMemoryResourceNotification);
	HANDLE high = CreateMemoryResourceNotification(HighMemoryResourceNotification);
	TEST_CHECK(low != NULL && high != NULL);
	BOOL lowState = query(low), highState = query(high);
	TEST_CHECK_EQ(lowState ? WAIT_OBJECT_0 : WAIT_TIMEOUT, WaitForSingleObject(low, 0));
	TEST_CHECK_EQ(highState ? WAIT_OBJECT_0 : WAIT_TIMEOUT, WaitForSingleObject(high, 0));
	HANDLE copy = NULL;
	TEST_CHECK(DuplicateHandle(GetCurrentProcess(), low, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS));
	TEST_CHECK_EQ(lowState, query(copy));
	HANDLE restricted = NULL;
	TEST_CHECK(DuplicateHandle(GetCurrentProcess(), low, GetCurrentProcess(), &restricted, 1, FALSE, 0));
	TEST_CHECK_EQ(WAIT_FAILED, WaitForSingleObject(restricted, 0));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK_EQ(WAIT_FAILED, WaitForMultipleObjects(1, &restricted, FALSE, 0));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(CloseHandle(restricted));
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(event != NULL);
	TEST_CHECK_EQ(FALSE, query(event));
	TEST_CHECK(SetEvent(event));
	TEST_CHECK_EQ(TRUE, query(event));
	TEST_CHECK(CloseHandle(event));
	if (synthetic) {
		TEST_CHECK_EQ(FALSE, lowState);
		TEST_CHECK_EQ(FALSE, highState);
		writeState("1,0");
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(low, 2000));
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(copy, 0));
		TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(high, 0));
		TEST_CHECK_EQ(TRUE, query(copy));
		writeState("0,0");
		TEST_CHECK_EQ(FALSE, query(low));
		TEST_CHECK_EQ(FALSE, query(high));
		writeState("0,1");
		HANDLE handles[2] = {low, high};
		TEST_CHECK_EQ(WAIT_OBJECT_0 + 1, WaitForMultipleObjects(2, handles, FALSE, 2000));
		TEST_CHECK_EQ(FALSE, query(low));
		TEST_CHECK_EQ(TRUE, query(high));
		TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(low, 0));
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(high, 0));
		TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForMultipleObjects(2, handles, TRUE, 0));
		event = CreateEventW(NULL, TRUE, TRUE, NULL);
		TEST_CHECK(event != NULL);
		HANDLE all[2] = {high, event};
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjects(2, all, TRUE, 0));
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(high, 0));
		TEST_CHECK(CloseHandle(event));
		writeState("0,0");
		TEST_CHECK_EQ(FALSE, query(low));
		TEST_CHECK_EQ(FALSE, query(high));
		TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForMultipleObjects(2, handles, FALSE, 0));
		writeState("failed");
		TEST_CHECK_EQ(WAIT_FAILED, WaitForMultipleObjects(2, handles, FALSE, 2000));
		TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
		unchanged = 7;
		TEST_CHECK(!QueryMemoryResourceNotification(low, &unchanged));
		TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
		TEST_CHECK_EQ(7, unchanged);
		TEST_CHECK_EQ(WAIT_FAILED, WaitForSingleObject(copy, 0));
		TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	}
	TEST_CHECK(CloseHandle(low));
	TEST_CHECK(CloseHandle(copy));
	TEST_CHECK(CloseHandle(high));
	if (synthetic) {
		writeState("0,1");
		high = CreateMemoryResourceNotification(HighMemoryResourceNotification);
		TEST_CHECK(high != NULL);
		TEST_CHECK_EQ(TRUE, query(high));
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(high, 0));
		TEST_CHECK(CloseHandle(high));
		writeState("0,0");
		low = CreateMemoryResourceNotification(LowMemoryResourceNotification);
		TEST_CHECK(low != NULL);
		writeState("slow");
		TEST_CHECK_EQ(WAIT_FAILED, WaitForSingleObject(low, 2000));
		TEST_CHECK_EQ(ERROR_TIMEOUT, GetLastError());
		TEST_CHECK(CloseHandle(low));
		writeState("0,0");
		for (unsigned i = 0; i < 8; ++i) {
			low = CreateMemoryResourceNotification(LowMemoryResourceNotification);
			TEST_CHECK(low != NULL);
			TEST_CHECK(CloseHandle(low));
		}
		TEST_CHECK(DeleteFileA(stateFile));
	}
	return 0;
}
