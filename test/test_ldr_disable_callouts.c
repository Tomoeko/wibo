#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef LONG(WINAPI *disable_callouts_fn)(PVOID);
typedef LONG (*count_fn)(void);

static FARPROC resolve(HMODULE module, const char *name) {
	FARPROC address = GetProcAddress(module, name);
	TEST_CHECK_MSG(address != NULL, "Missing export %s", name);
	return address;
}

static count_fn resolve_count(HMODULE module, const char *name) {
	FARPROC address = resolve(module, name);
	count_fn function;
	_Static_assert(sizeof(function) == sizeof(address), "Function pointer size");
	memcpy(&function, &address, sizeof(function));
	return function;
}

static DWORD WINAPI worker(void *argument) {
	(void)argument;
	return 0;
}

static void run_worker(void) {
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	TEST_CHECK(thread != NULL);
	DWORD result = WaitForSingleObject(thread, 3000);
	if (result != WAIT_OBJECT_0)
		ExitProcess(90);
	TEST_CHECK(CloseHandle(thread));
}

static LONG disable(disable_callouts_fn function, const char *label, HMODULE module) {
	SetLastError(0x4321);
	LONG status = function(module);
	DWORD error = GetLastError();
	printf("%s status=%08lx error=%lu\n", label, (unsigned long)(ULONG)status, (unsigned long)error);
	TEST_CHECK_EQ(0x4321, error);
	return status;
}

int main(void) {
	FARPROC address = resolve(GetModuleHandleA("ntdll.dll"), "LdrDisableThreadCalloutsForDll");
	disable_callouts_fn disable_callouts;
	_Static_assert(sizeof(disable_callouts) == sizeof(address), "Function pointer size");
	memcpy(&disable_callouts, &address, sizeof(disable_callouts));

	HMODULE basic = LoadLibraryA("thread_callouts.dll");
	HMODULE tls = LoadLibraryA("thread_notifications.dll");
	TEST_CHECK(basic != NULL && tls != NULL);
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)basic;
	const IMAGE_NT_HEADERS *headers = (const IMAGE_NT_HEADERS *)((const BYTE *)basic + dos->e_lfanew);
	TEST_CHECK_EQ(0, headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size);
	dos = (const IMAGE_DOS_HEADER *)tls;
	headers = (const IMAGE_NT_HEADERS *)((const BYTE *)tls + dos->e_lfanew);
	TEST_CHECK(headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size != 0);

	count_fn basic_attach = resolve_count(basic, "get_thread_attach_count");
	count_fn basic_detach = resolve_count(basic, "get_thread_detach_count");
	count_fn tls_attach = resolve_count(tls, "get_thread_attach_count");
	count_fn tls_detach = resolve_count(tls, "get_thread_detach_count");
	run_worker();
	TEST_CHECK_EQ(1, basic_attach());
	TEST_CHECK_EQ(1, basic_detach());
	TEST_CHECK_EQ(1, tls_attach());
	TEST_CHECK_EQ(1, tls_detach());

	TEST_CHECK_EQ((LONG)0xc0000135, disable(disable_callouts, "null", NULL));
	TEST_CHECK_EQ((LONG)0xc0000135, disable(disable_callouts, "static-tls", tls));
	TEST_CHECK_EQ(0, disable(disable_callouts, "ordinary", basic));
	TEST_CHECK_EQ(0, disable(disable_callouts, "repeated", basic));
	run_worker();
	TEST_CHECK_EQ(1, basic_attach());
	TEST_CHECK_EQ(1, basic_detach());
	TEST_CHECK_EQ(2, tls_attach());
	TEST_CHECK_EQ(2, tls_detach());
	TEST_CHECK(FreeLibrary(tls));
	TEST_CHECK(FreeLibrary(basic));
	puts("thread callout control passed");
	return 0;
}
