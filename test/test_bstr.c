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
	VARIANT value;
	memset(&value, 0x55, sizeof(value));
	VariantInit(&value);
	TEST_CHECK_EQ(VT_EMPTY, V_VT(&value));
	V_VT(&value) = VT_BSTR;
	V_BSTR(&value) = SysAllocStringLen(source, 5);
	TEST_CHECK_EQ(S_OK, VariantClear(&value));
	TEST_CHECK_EQ(VT_EMPTY, V_VT(&value));
	text = SysAllocString(L"retained");
	V_VT(&value) = VT_BSTR | VT_BYREF;
	V_BSTRREF(&value) = &text;
	TEST_CHECK_EQ(S_OK, VariantClear(&value));
	TEST_CHECK_EQ(8, SysStringLen(text));
	SysFreeString(text);
	V_VT(&value) = VT_I8;
	V_I8(&value) = -0x100000001LL;
	TEST_CHECK_EQ(S_OK, VariantClear(&value));
	TEST_CHECK_EQ(VT_EMPTY, V_VT(&value));
	V_VT(&value) = 0x123;
	TEST_CHECK_EQ((HRESULT)DISP_E_BADVARTYPE, VariantClear(&value));
	TEST_CHECK_EQ(0x123, V_VT(&value));
	VariantInit(&value);
	V_VT(&value) = VT_BSTR;
	V_BSTR(&value) = SysAllocStringLen(source, 5);
	TEST_CHECK_EQ(S_OK, VariantChangeType(&value, &value, 0, VT_BSTR));
	TEST_CHECK_EQ(5, SysStringLen(V_BSTR(&value)));
	TEST_CHECK(memcmp(source, V_BSTR(&value), sizeof(source)) == 0);
	VariantClear(&value);
	V_VT(&value) = VT_I8;
	V_I8(&value) = -0x100000001LL;
	TEST_CHECK_EQ(S_OK, VariantChangeType(&value, &value, 0, VT_BSTR));
	TEST_CHECK(wcscmp(V_BSTR(&value), L"-4294967297") == 0);
	VariantClear(&value);
	V_VT(&value) = VT_BOOL;
	V_BOOL(&value) = VARIANT_TRUE;
	TEST_CHECK_EQ(S_OK, VariantChangeType(&value, &value, VARIANT_ALPHABOOL, VT_BSTR));
	TEST_CHECK(wcscmp(V_BSTR(&value), L"True") == 0);
	VariantClear(&value);
	HMODULE module = GetModuleHandleA("oleaut32.dll");
	TEST_CHECK(module != NULL);
	TEST_CHECK(GetProcAddress(module, (LPCSTR)(uintptr_t)2) == GetProcAddress(module, "SysAllocString"));
	TEST_CHECK(GetProcAddress(module, (LPCSTR)(uintptr_t)4) == GetProcAddress(module, "SysAllocStringLen"));
	TEST_CHECK(GetProcAddress(module, (LPCSTR)(uintptr_t)6) == GetProcAddress(module, "SysFreeString"));
	TEST_CHECK(GetProcAddress(module, (LPCSTR)(uintptr_t)7) == GetProcAddress(module, "SysStringLen"));
	TEST_CHECK(GetProcAddress(module, (LPCSTR)(uintptr_t)8) == GetProcAddress(module, "VariantInit"));
	TEST_CHECK(GetProcAddress(module, (LPCSTR)(uintptr_t)9) == GetProcAddress(module, "VariantClear"));
	return 0;
}
