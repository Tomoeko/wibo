#include "test_assert.h"
#include <stdint.h>
#include <wchar.h>
#include <windows.h>

#include <oleauto.h>

int main(void) {
	TEST_CHECK(SysAllocString(NULL) == NULL);
	TEST_CHECK_EQ(0, SysStringLen(NULL));
	SysFreeString(NULL);
	SetLastError(0x4321);
	BSTR empty = SysAllocString(L"");
	TEST_CHECK(empty != NULL);
	TEST_CHECK_EQ(0, SysStringLen(empty));
	TEST_CHECK_EQ(0, empty[0]);
	TEST_CHECK_EQ(0x4321, GetLastError());
	SysFreeString(empty);
	const WCHAR source[] = {L'a', 0, 0x4E2D, 0xD83D, 0xDE00};
	BSTR text = SysAllocStringLen(source, 5);
	TEST_CHECK(text != NULL);
	TEST_CHECK_EQ(5, SysStringLen(text));
	TEST_CHECK_EQ(10, ((DWORD *)text)[-1]);
	TEST_CHECK(memcmp(source, text, sizeof(source)) == 0);
	TEST_CHECK_EQ(0, text[5]);
	SysFreeString(text);
	text = SysAllocString(L"sample");
	TEST_CHECK(text != NULL && wcscmp(text, L"sample") == 0);
	TEST_CHECK_EQ(6, SysStringLen(text));
	SysFreeString(text);
	text = SysAllocStringLen(NULL, 3);
	TEST_CHECK(text != NULL);
	TEST_CHECK_EQ(3, SysStringLen(text));
	TEST_CHECK_EQ(0, text[3]);
	SysFreeString(text);
	HMODULE module = GetModuleHandleA("oleaut32.dll");
	TEST_CHECK(module != NULL);
	TEST_CHECK(GetProcAddress(module, (LPCSTR)(uintptr_t)2) == GetProcAddress(module, "SysAllocString"));
	TEST_CHECK(GetProcAddress(module, (LPCSTR)(uintptr_t)4) == GetProcAddress(module, "SysAllocStringLen"));
	TEST_CHECK(GetProcAddress(module, (LPCSTR)(uintptr_t)6) == GetProcAddress(module, "SysFreeString"));
	TEST_CHECK(GetProcAddress(module, (LPCSTR)(uintptr_t)7) == GetProcAddress(module, "SysStringLen"));
	return 0;
}
