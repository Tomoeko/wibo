#define _WIN32_WINNT 0x0601
#include <stdio.h>
#include <windows.h>

#include "test_assert.h"

static int inspect(HANDLE handle, DWORD expected) {
	DWORD flags = 0x12345678;
	if (!GetHandleInformation(handle, &flags) || flags != expected) {
		fprintf(stderr, "handle flags expected=%lu actual=%lu error=%lu\n", (unsigned long)expected,
				(unsigned long)flags, (unsigned long)GetLastError());
		return 0;
	}
	return 1;
}

static int checkCase(BOOL inherit, BOOL supplied) {
	SECURITY_ATTRIBUTES security = {sizeof(security), NULL, inherit};
	HANDLE readHandle = INVALID_HANDLE_VALUE, writeHandle = INVALID_HANDLE_VALUE;
	if (!CreatePipe(&readHandle, &writeHandle, supplied ? &security : NULL, 0))
		return 0;
	DWORD expected = supplied && inherit ? HANDLE_FLAG_INHERIT : 0;
	int success = inspect(readHandle, expected) && inspect(writeHandle, expected);
	if (!SetHandleInformation(readHandle, HANDLE_FLAG_INHERIT, expected ^ HANDLE_FLAG_INHERIT) ||
		!inspect(readHandle, expected ^ HANDLE_FLAG_INHERIT) || !inspect(writeHandle, expected))
		success = 0;
	int readClosed = CloseHandle(readHandle) != FALSE;
	int writeClosed = CloseHandle(writeHandle) != FALSE;
	return success && readClosed && writeClosed;
}

int main(void) {
	int absent = checkCase(FALSE, FALSE);
	int disabled = checkCase(FALSE, TRUE);
	int enabled = checkCase(TRUE, TRUE);
	TEST_CHECK(absent && disabled && enabled);
	return 0;
}
