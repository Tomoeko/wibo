#include "test_assert.h"
#include <ole2.h>
#include <windows.h>

static DWORD WINAPI worker(LPVOID unused) {
	(void)unused;
	TEST_CHECK_EQ(S_OK, CoInitializeEx(NULL, COINIT_MULTITHREADED));
	TEST_CHECK_EQ(RPC_E_CHANGED_MODE, OleInitialize(NULL));
	OleUninitialize();
	TEST_CHECK_EQ(S_FALSE, CoInitializeEx(NULL, COINIT_MULTITHREADED));
	CoUninitialize();
	CoUninitialize();
	TEST_CHECK_EQ(S_OK, OleInitialize(NULL));
	OleUninitialize();
	return 0;
}

int main(void) {
	TEST_CHECK_EQ(S_OK, CoInitialize(NULL));
	TEST_CHECK_EQ(S_OK, OleInitialize(NULL));
	TEST_CHECK_EQ(S_FALSE, OleInitialize(NULL));
	OleUninitialize();
	TEST_CHECK_EQ(RPC_E_CHANGED_MODE, CoInitializeEx(NULL, COINIT_MULTITHREADED));
	OleUninitialize();
	OleUninitialize();
	TEST_CHECK_EQ(S_FALSE, CoInitialize(NULL));
	CoUninitialize();
	CoUninitialize();
	TEST_CHECK_EQ(S_OK, CoInitializeEx(NULL, COINIT_MULTITHREADED));
	CoUninitialize();
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, INFINITE));
	DWORD result = 1;
	TEST_CHECK(GetExitCodeThread(thread, &result));
	TEST_CHECK_EQ(0, result);
	CloseHandle(thread);
	return 0;
}
