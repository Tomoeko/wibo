#include "test_assert.h"
#include <windows.h>

int main(void) {
	HMODULE module = LoadLibraryW(L"thread_release.dll");
	TEST_CHECK(module != NULL);
	TEST_CHECK(LoadLibraryW(L"thread_release.dll") == module);
	FARPROC proc = GetProcAddress(module, "release_worker");
#ifndef _WIN64
	if (!proc)
		proc = GetProcAddress(module, "release_worker@4");
	if (!proc)
		proc = GetProcAddress(module, "_release_worker@4");
#endif
	TEST_CHECK(proc != NULL);
	LONG detachCount = 0;
	for (unsigned i = 0; i < 2; ++i) {
		HANDLE thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(ULONG_PTR)proc, &detachCount, 0, NULL);
		TEST_CHECK(thread != NULL);
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
		DWORD code = 0;
		TEST_CHECK(GetExitCodeThread(thread, &code));
		TEST_CHECK_EQ(37, code);
		TEST_CHECK(CloseHandle(thread));
		TEST_CHECK_EQ(i, detachCount);
		TEST_CHECK(GetModuleHandleW(L"thread_release.dll") == (i == 0 ? module : NULL));
	}
	return 0;
}
