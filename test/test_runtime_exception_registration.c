#include <windows.h>

#include "test_assert.h"

typedef HRESULT(WINAPI *register_fn)(PCWSTR, PVOID);

int main(void) {
	HMODULE module = GetModuleHandleW(L"kernel32.dll");
	register_fn registration = (register_fn)(ULONG_PTR)GetProcAddress(module, "WerRegisterRuntimeExceptionModule");
	TEST_CHECK(registration != NULL);
	const WCHAR name[] = L"synthetic-exception-reporter.dll";
	DWORD context = 42;
	SetLastError(0x20001234);
	HRESULT result = registration(name, &context);
	TEST_CHECK_EQ(0x20001234, GetLastError());
	TEST_CHECK_EQ(42, context);
	if (getenv("WIBO_EXPECT_WER_UNSUPPORTED")) {
		TEST_CHECK_EQ(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), result);
	} else {
		TEST_CHECK_EQ(S_OK, result);
		register_fn unregister = (register_fn)(ULONG_PTR)GetProcAddress(module, "WerUnregisterRuntimeExceptionModule");
		TEST_CHECK(unregister != NULL);
		TEST_CHECK_EQ(S_OK, unregister(name, &context));
	}
	return 0;
}
