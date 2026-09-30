#define _WIN32_WINNT 0x0601
#include <objbase.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

static const WCHAR prog_id[] = L"WiboFixture.Component";
static const WCHAR guid_text[] = L"{A30F1D51-54BE-41A0-A187-9F6B09327540}";
static const WCHAR prog_key[] = L"WiboFixture.Component\\CLSID";
static const WCHAR clsid_key[] = L"CLSID\\{A30F1D51-54BE-41A0-A187-9F6B09327540}\\ProgID";

static void cleanup(void) {
	RegDeleteKeyW(HKEY_CLASSES_ROOT, prog_key);
	RegDeleteKeyW(HKEY_CLASSES_ROOT, L"WiboFixture.Component");
	RegDeleteKeyW(HKEY_CLASSES_ROOT, clsid_key);
	RegDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\{A30F1D51-54BE-41A0-A187-9F6B09327540}");
}

static void set_default_string(const WCHAR *path, const WCHAR *value) {
	HKEY key = NULL;
	TEST_CHECK_EQ(ERROR_SUCCESS,
				  RegCreateKeyExW(HKEY_CLASSES_ROOT, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL));
	const DWORD bytes = (lstrlenW(value) + 1) * sizeof(WCHAR);
	TEST_CHECK_EQ(ERROR_SUCCESS, RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)value, bytes));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(key));
}

int main(void) {
	cleanup();
	TEST_CHECK_EQ(0, atexit(cleanup));
	set_default_string(prog_key, guid_text);
	set_default_string(clsid_key, prog_id);

	GUID expected = {0};
	TEST_CHECK_EQ(S_OK, CLSIDFromString(guid_text, &expected));
	GUID actual = {0};
	TEST_CHECK_EQ(S_OK, CLSIDFromProgID(prog_id, &actual));
	TEST_CHECK(memcmp(&expected, &actual, sizeof(actual)) == 0);
	TEST_CHECK_EQ(S_OK, CLSIDFromProgID(L"wibofixture.component", &actual));
	TEST_CHECK(memcmp(&expected, &actual, sizeof(actual)) == 0);

	LPOLESTR allocated = NULL;
	TEST_CHECK_EQ(S_OK, ProgIDFromCLSID(&expected, &allocated));
	TEST_CHECK(allocated != NULL && lstrcmpW(allocated, prog_id) == 0);
	CoTaskMemFree(allocated);

	GUID missing = {0x42aa4137, 0x8d18, 0x4e9d, {0xa1, 0xb3, 0xd1, 0x8f, 0x75, 0xfa, 0x20, 0xcc}};
	allocated = (LPOLESTR)0x1234;
	TEST_CHECK_EQ(REGDB_E_CLASSNOTREG, ProgIDFromCLSID(&missing, &allocated));
	TEST_CHECK(allocated == NULL);

	set_default_string(prog_key, L"invalid-guid");
	TEST_CHECK_EQ(CO_E_CLASSSTRING, CLSIDFromProgID(prog_id, &actual));
	return 0;
}
