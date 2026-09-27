#include "test_assert.h"
#include <ole2.h>
#include <roapi.h>
#include <windows.h>

typedef HRESULT(WINAPI *InitializeRuntimeFn)(RO_INIT_TYPE);
typedef void(WINAPI *UninitializeRuntimeFn)(void);

static InitializeRuntimeFn initializeRuntime;
static UninitializeRuntimeFn uninitializeRuntime;

static DWORD WINAPI worker(LPVOID unused) {
	(void)unused;
	TEST_CHECK_EQ(S_OK, initializeRuntime(RO_INIT_MULTITHREADED));
	TEST_CHECK_EQ(RPC_E_CHANGED_MODE, initializeRuntime(RO_INIT_SINGLETHREADED));
	TEST_CHECK_EQ(S_FALSE, CoInitializeEx(NULL, COINIT_MULTITHREADED));
	CoUninitialize();
	uninitializeRuntime();
	TEST_CHECK_EQ(S_OK, initializeRuntime(RO_INIT_SINGLETHREADED));
	uninitializeRuntime();
	return 0;
}

int main(void) {
	HMODULE module = LoadLibraryW(L"combase.dll");
	TEST_CHECK(module != NULL);
	initializeRuntime = (InitializeRuntimeFn)(void *)GetProcAddress(module, "RoInitialize");
	uninitializeRuntime = (UninitializeRuntimeFn)(void *)GetProcAddress(module, "RoUninitialize");
	TEST_CHECK(initializeRuntime != NULL);
	TEST_CHECK(uninitializeRuntime != NULL);

	TEST_CHECK_EQ(S_OK, CoInitializeEx(NULL, COINIT_MULTITHREADED));
	TEST_CHECK_EQ(S_FALSE, initializeRuntime(RO_INIT_MULTITHREADED));
	uninitializeRuntime();
	TEST_CHECK_EQ(RPC_E_CHANGED_MODE, initializeRuntime(RO_INIT_SINGLETHREADED));
	TEST_CHECK_EQ(S_FALSE, CoInitializeEx(NULL, COINIT_MULTITHREADED));
	CoUninitialize();
	CoUninitialize();

	TEST_CHECK_EQ(S_OK, initializeRuntime(RO_INIT_SINGLETHREADED));
	TEST_CHECK_EQ(S_FALSE, initializeRuntime(RO_INIT_SINGLETHREADED));
	TEST_CHECK_EQ(S_FALSE, CoInitialize(NULL));
	TEST_CHECK_EQ(S_OK, OleInitialize(NULL));
	TEST_CHECK_EQ(RPC_E_CHANGED_MODE, initializeRuntime(RO_INIT_MULTITHREADED));
	uninitializeRuntime();
	CoUninitialize();
	OleUninitialize();
	TEST_CHECK_EQ(RPC_E_CHANGED_MODE, CoInitializeEx(NULL, COINIT_MULTITHREADED));
	uninitializeRuntime();

	TEST_CHECK_EQ(S_OK, initializeRuntime(RO_INIT_MULTITHREADED));
	TEST_CHECK_EQ(S_FALSE, CoInitializeEx(NULL, COINIT_MULTITHREADED));
	CoUninitialize();
	uninitializeRuntime();

	TEST_CHECK_EQ(S_OK, initializeRuntime(RO_INIT_MULTITHREADED));
	CoUninitialize();
	TEST_CHECK_EQ(S_OK, CoInitialize(NULL));
	uninitializeRuntime();
	TEST_CHECK_EQ(S_OK, CoInitializeEx(NULL, COINIT_MULTITHREADED));
	CoUninitialize();

	TEST_CHECK_EQ(S_OK, initializeRuntime(RO_INIT_SINGLETHREADED));
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	DWORD result = 1;
	TEST_CHECK(GetExitCodeThread(thread, &result));
	TEST_CHECK_EQ(0, result);
	CloseHandle(thread);
	TEST_CHECK_EQ(RPC_E_CHANGED_MODE, initializeRuntime(RO_INIT_MULTITHREADED));
	uninitializeRuntime();
	TEST_CHECK_EQ(S_OK, initializeRuntime(RO_INIT_MULTITHREADED));
	uninitializeRuntime();
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
