#include <windows.h>

#include "test_assert.h"

// Only an independently created, owned per-user subtree is modified. The Wine
// runner deletes the reported subtree; wibo's registry is process-local.
// ANSI cross-calls use ASCII: the shim's existing ACP mapping is not a full
// Windows code-page implementation. Wide-only data includes non-ASCII UTF-16.
// https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regsetvalueexa
// https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regqueryvalueexa
static const DWORD last_error_sentinel = 0x13572468;
static unsigned int sets, queries;

typedef struct {
	DWORD before;
	DWORD value;
	DWORD after;
} GuardedDword;

static void check_status(LONG expected, LONG result) {
	DWORD error = GetLastError();
	TEST_CHECK_EQ(expected, result);
	TEST_CHECK_EQ(last_error_sentinel, error);
}

static void set_a(HKEY key, const char *name, DWORD type, const void *data, DWORD size) {
	SetLastError(last_error_sentinel);
	check_status(ERROR_SUCCESS, RegSetValueExA(key, name, 0, type, data, size));
	++sets;
}

static void set_w(HKEY key, const WCHAR *name, DWORD type, const void *data, DWORD size) {
	SetLastError(last_error_sentinel);
	check_status(ERROR_SUCCESS, RegSetValueExW(key, name, 0, type, data, size));
	++sets;
}

static LONG query(BOOL wide, HKEY key, const void *name, DWORD *type, BYTE *data, DWORD *size) {
	return wide ? RegQueryValueExW(key, name, NULL, type, data, size)
				: RegQueryValueExA(key, name, NULL, type, data, size);
}

static void check_value(BOOL wide, HKEY key, const void *name, DWORD expected_type, const void *expected,
						DWORD expected_size) {
	const DWORD capacities[] = {0, expected_size ? expected_size - 1 : 0, expected_size, expected_size + 4};
	GuardedDword size = {0x12345678, 0xabcdef, 0x76543210};
	GuardedDword type = {0x11223344, 0xabcdef, 0x44332211};
	SetLastError(last_error_sentinel);
	check_status(ERROR_SUCCESS, query(wide, key, name, &type.value, NULL, &size.value));
	TEST_CHECK_EQ(expected_type, type.value);
	TEST_CHECK_EQ(expected_size, size.value);
	++queries;
	SetLastError(last_error_sentinel);
	check_status(ERROR_SUCCESS, query(wide, key, name, &type.value, NULL, NULL));
	TEST_CHECK_EQ(expected_type, type.value);
	++queries;
	SetLastError(last_error_sentinel);
	check_status(ERROR_SUCCESS, query(wide, key, name, NULL, NULL, NULL));
	++queries;
	for (unsigned int i = 0; i < sizeof(capacities) / sizeof(capacities[0]); ++i) {
		BYTE storage[132];
		memset(storage, 0xcc, sizeof(storage));
		TEST_CHECK(capacities[i] <= sizeof(storage) - 2);
		size.value = capacities[i];
		type.value = 0xabcdef;
		SetLastError(last_error_sentinel);
		check_status(capacities[i] < expected_size ? ERROR_MORE_DATA : ERROR_SUCCESS,
					 query(wide, key, name, &type.value, storage + 1, &size.value));
		TEST_CHECK_EQ(expected_type, type.value);
		TEST_CHECK_EQ(expected_size, size.value);
		TEST_CHECK_EQ(0xcc, storage[0]);
		for (unsigned int j = 1 + capacities[i]; j < sizeof(storage); ++j)
			TEST_CHECK_EQ(0xcc, storage[j]);
		if (capacities[i] >= expected_size && expected_size) {
			TEST_CHECK(memcmp(storage + 1, expected, expected_size) == 0);
		}
		++queries;
	}
	TEST_CHECK_EQ(0x12345678, size.before);
	TEST_CHECK_EQ(0x76543210, size.after);
	TEST_CHECK_EQ(0x11223344, type.before);
	TEST_CHECK_EQ(0x44332211, type.after);
}

static void check_both(HKEY key, const char *name_a, const WCHAR *name_w, DWORD type, const void *data, DWORD size) {
	check_value(FALSE, key, name_a, type, data, size);
	check_value(TRUE, key, name_w, type, data, size);
}

static HKEY create_owned(HKEY parent, const char *name) {
	HKEY key = NULL;
	DWORD disposition = 0;
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCreateKeyExA(parent, name, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, &disposition));
	TEST_CHECK_EQ(REG_CREATED_NEW_KEY, disposition);
	return key;
}

int main(void) {
	char owned_name[96];
	int count =
		snprintf(owned_name, sizeof(owned_name), "WiboRegValues_%08lx_%08lx", GetCurrentProcessId(), GetTickCount());
	TEST_CHECK(count > 0 && (size_t)count < sizeof(owned_name));
	HKEY key = create_owned(HKEY_CURRENT_USER, owned_name);
	printf("owned_key=%s\n", owned_name);
	fflush(stdout);
	HKEY second = NULL;
	TEST_CHECK_EQ(ERROR_SUCCESS, RegOpenKeyExA(HKEY_CURRENT_USER, owned_name, 0, KEY_ALL_ACCESS, &second));
	TEST_CHECK(second != key);
	DWORD number = 0x89abcdef;
	const BYTE binary[] = {0, 0xff, 0x80, 1, 0, 7, 42};
	const ULONGLONG qword = 0xfedcba9876543210ULL;
	set_a(key, "NuMbEr", REG_DWORD, &number, sizeof(number));
	check_both(second, "number", L"NUMBER", REG_DWORD, &number, sizeof(number));
	set_w(second, L"BiNaRy", REG_BINARY, binary, sizeof(binary));
	check_both(key, "binary", L"BINARY", REG_BINARY, binary, sizeof(binary));
	set_w(key, L"Qword", REG_QWORD, &qword, sizeof(qword));
	check_both(second, "QWORD", L"qword", REG_QWORD, &qword, sizeof(qword));
	set_a(key, NULL, REG_DWORD, &number, sizeof(number));
	check_both(second, "", NULL, REG_DWORD, &number, sizeof(number));
	set_w(second, L"", REG_BINARY, binary, sizeof(binary));
	check_both(key, NULL, L"", REG_BINARY, binary, sizeof(binary));
	// Replacement changes both type and bytes, and is visible through another handle.
	set_w(second, L"NUMBER", REG_BINARY, binary, sizeof(binary));
	check_both(key, "Number", L"number", REG_BINARY, binary, sizeof(binary));
	const char text[] = "Alpha beta";
	const WCHAR text_w[] = L"Alpha beta";
	set_a(key, "Text", REG_SZ, text, sizeof(text));
	check_value(FALSE, second, "TEXT", REG_SZ, text, sizeof(text));
	check_value(TRUE, second, L"text", REG_SZ, text_w, sizeof(text_w));
	const char expand[] = "%WIBO_REG_CONTROL%/unchanged";
	const WCHAR expand_w[] = L"%WIBO_REG_CONTROL%/unchanged";
	set_w(second, L"Expand", REG_EXPAND_SZ, expand_w, sizeof(expand_w));
	check_value(FALSE, key, "expand", REG_EXPAND_SZ, expand, sizeof(expand));
	check_value(TRUE, key, L"EXPAND", REG_EXPAND_SZ, expand_w, sizeof(expand_w));
	const char multi[] = "one\0two\0";
	const WCHAR multi_w[] = L"one\0two\0";
	set_a(key, "Multi", REG_MULTI_SZ, multi, sizeof(multi));
	check_value(FALSE, second, "multi", REG_MULTI_SZ, multi, sizeof(multi));
	check_value(TRUE, second, L"MULTI", REG_MULTI_SZ, multi_w, sizeof(multi_w));
	const WCHAR unicode[] = {0x03a9, 0x4e2d, 0xd83d, 0xde00, 0};
	set_w(key, L"WideOnly", REG_SZ, unicode, sizeof(unicode));
	check_value(TRUE, second, L"wideonly", REG_SZ, unicode, sizeof(unicode));
	const WCHAR unicode_name[] = {0x4e2d, 0};
	set_w(key, unicode_name, REG_BINARY, binary, sizeof(binary));
	check_value(TRUE, second, unicode_name, REG_BINARY, binary, sizeof(binary));
	set_a(key, "EmptyBinary", REG_BINARY, NULL, 0);
	check_both(second, "emptybinary", L"EMPTYBINARY", REG_BINARY, NULL, 0);
	set_w(key, L"None", REG_NONE, binary, sizeof(binary));
	check_both(second, "none", L"NONE", REG_NONE, binary, sizeof(binary));
	// Slash and backslash are literal value-name characters, not key components.
	set_a(key, "path/name", REG_DWORD, &number, sizeof(number));
	set_w(key, L"path\\name", REG_BINARY, binary, sizeof(binary));
	check_both(second, "PATH/NAME", L"path/name", REG_DWORD, &number, sizeof(number));
	check_both(second, "PATH\\NAME", L"path\\name", REG_BINARY, binary, sizeof(binary));
	HKEY child = create_owned(key, "Separate");
	DWORD child_number = 42;
	set_a(child, "NUMBER", REG_DWORD, &child_number, sizeof(child_number));
	check_both(child, "number", L"number", REG_DWORD, &child_number, sizeof(child_number));
	check_both(second, "number", L"number", REG_BINARY, binary, sizeof(binary));
	// Error calls use owned buffers and documented NULL argument combinations.
	BYTE bytes[8];
	DWORD size = sizeof(bytes), type = 0, reserved = 0;
	memset(bytes, 0xcc, sizeof(bytes));
	SetLastError(last_error_sentinel);
	check_status(ERROR_FILE_NOT_FOUND, RegQueryValueExA(key, "Absent", NULL, &type, bytes, &size));
	SetLastError(last_error_sentinel);
	check_status(ERROR_FILE_NOT_FOUND, RegQueryValueExW(key, L"Absent", NULL, NULL, NULL, NULL));
	SetLastError(last_error_sentinel);
	check_status(ERROR_INVALID_PARAMETER, RegQueryValueExA(key, "number", &reserved, &type, bytes, &size));
	SetLastError(last_error_sentinel);
	check_status(ERROR_INVALID_PARAMETER, RegQueryValueExW(key, L"number", &reserved, &type, bytes, &size));
	SetLastError(last_error_sentinel);
	check_status(ERROR_INVALID_PARAMETER, RegQueryValueExA(key, "number", NULL, &type, bytes, NULL));
	SetLastError(last_error_sentinel);
	check_status(ERROR_INVALID_PARAMETER, RegQueryValueExW(key, L"number", NULL, &type, bytes, NULL));
	SetLastError(last_error_sentinel);
	check_status(ERROR_INVALID_HANDLE, RegSetValueExA(NULL, "number", 0, REG_DWORD, (BYTE *)&number, sizeof(number)));
	SetLastError(last_error_sentinel);
	check_status(ERROR_INVALID_HANDLE, RegSetValueExW(NULL, L"number", 0, REG_DWORD, (BYTE *)&number, sizeof(number)));
	SetLastError(last_error_sentinel);
	check_status(ERROR_INVALID_HANDLE, RegQueryValueExA(NULL, "number", NULL, &type, NULL, NULL));
	SetLastError(last_error_sentinel);
	check_status(ERROR_INVALID_HANDLE, RegQueryValueExW(NULL, L"number", NULL, &type, NULL, NULL));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(child));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(key));
	// Closing one alias must not erase the value data.
	check_both(second, "number", L"number", REG_BINARY, binary, sizeof(binary));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(second));
	printf("registry-values: %u sets, %u queries, 10 error cases passed\n", sets, queries);
	return 0;
}
