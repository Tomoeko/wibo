#include <windows.h>

#include "test_assert.h"

// Null/empty names return the original handle; named opens use default access.
// https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regopenkeya
// Only ASCII names and ordinary per-user permissions are exercised here.
// The Wine runner deletes the reported, newly-created owned subtree. Wibo's
// registry is process-local; it currently has no key-deletion implementation.
static const DWORD last_error_sentinel = 0x13572468;
static unsigned int checked_opens;

typedef struct {
	DWORD before;
	HKEY result;
	DWORD after;
} GuardedResult;

static HKEY check_open_a(HKEY base, const char *name, LONG expected, BOOL same_handle) {
	GuardedResult out = {0x13579bdf, (HKEY)(ULONG_PTR)0x1234, 0x2468ace0};
	SetLastError(last_error_sentinel);
	LONG result = RegOpenKeyA(base, name, &out.result);
	DWORD error = GetLastError();
	TEST_CHECK_MSG(result == expected, "RegOpenKeyA(%s): %ld, expected %ld", name ? name : "NULL", result, expected);
	TEST_CHECK_EQ(last_error_sentinel, error);
	TEST_CHECK_EQ(0x13579bdf, out.before);
	TEST_CHECK_EQ(0x2468ace0, out.after);
	if (expected == ERROR_SUCCESS) {
		TEST_CHECK(out.result != NULL);
		TEST_CHECK(same_handle ? out.result == base : out.result != base);
	} else {
		TEST_CHECK(out.result == NULL);
	}
	++checked_opens;
	return out.result;
}

static HKEY check_open_w(HKEY base, const WCHAR *name, LONG expected, BOOL same_handle) {
	GuardedResult out = {0x13579bdf, (HKEY)(ULONG_PTR)0x1234, 0x2468ace0};
	SetLastError(last_error_sentinel);
	LONG result = RegOpenKeyW(base, name, &out.result);
	DWORD error = GetLastError();
	TEST_CHECK_MSG(result == expected, "RegOpenKeyW: %ld, expected %ld", result, expected);
	TEST_CHECK_EQ(last_error_sentinel, error);
	TEST_CHECK_EQ(0x13579bdf, out.before);
	TEST_CHECK_EQ(0x2468ace0, out.after);
	if (expected == ERROR_SUCCESS) {
		TEST_CHECK(out.result != NULL);
		TEST_CHECK(same_handle ? out.result == base : out.result != base);
	} else {
		TEST_CHECK(out.result == NULL);
	}
	++checked_opens;
	return out.result;
}

static HKEY create_owned_child(HKEY parent, const char *name) {
	HKEY child = NULL;
	DWORD disposition = 0;
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCreateKeyExA(parent, name, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL,
												 &child, &disposition));
	TEST_CHECK_EQ(REG_CREATED_NEW_KEY, disposition);
	TEST_CHECK(child != NULL);
	return child;
}

int main(void) {
	const HKEY predefined[] = {HKEY_CLASSES_ROOT, HKEY_CURRENT_CONFIG, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE,
							   HKEY_USERS};
	for (unsigned int i = 0; i < sizeof(predefined) / sizeof(predefined[0]); ++i) {
		check_open_a(predefined[i], NULL, ERROR_SUCCESS, TRUE);
		check_open_a(predefined[i], "", ERROR_SUCCESS, TRUE);
		check_open_w(predefined[i], NULL, ERROR_SUCCESS, TRUE);
		check_open_w(predefined[i], L"", ERROR_SUCCESS, TRUE);
	}
	char owned_name[96];
	int count = snprintf(owned_name, sizeof(owned_name), "WiboRegOpenLegacy_%08lx_%08lx", GetCurrentProcessId(),
						 GetTickCount());
	TEST_CHECK(count > 0 && (size_t)count < sizeof(owned_name));
	check_open_a(HKEY_CURRENT_USER, owned_name, ERROR_FILE_NOT_FOUND, FALSE);
	HKEY parent = create_owned_child(HKEY_CURRENT_USER, owned_name);
	// Printed only after NEW_KEY establishes ownership. Flush before any later
	// assertion so the external Wine runner can clean up even on test failure.
	printf("owned_key=%s\n", owned_name);
	fflush(stdout);
	check_open_a(parent, NULL, ERROR_SUCCESS, TRUE);
	check_open_a(parent, "", ERROR_SUCCESS, TRUE);
	check_open_w(parent, NULL, ERROR_SUCCESS, TRUE);
	check_open_w(parent, L"", ERROR_SUCCESS, TRUE);
	HKEY original_child = create_owned_child(parent, "MiXeDChild");
	HKEY child_a = check_open_a(parent, "mixedchild", ERROR_SUCCESS, FALSE);
	HKEY child_w = check_open_w(parent, L"MIXEDCHILD", ERROR_SUCCESS, FALSE);
	TEST_CHECK(child_a != original_child && child_w != original_child && child_a != child_w);
	check_open_a(child_a, NULL, ERROR_SUCCESS, TRUE);
	check_open_w(child_w, L"", ERROR_SUCCESS, TRUE);
	// A default-security legacy open has sufficient access to create ordinary
	// descendants. This is not a test of custom DACL enforcement.
	HKEY grandchild_a = create_owned_child(child_a, "ViaAnsiOpen");
	HKEY grandchild_w = create_owned_child(child_w, "ViaWideOpen");
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(grandchild_a));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(grandchild_w));
	check_open_a(parent, "AbsentChild", ERROR_FILE_NOT_FOUND, FALSE);
	check_open_w(parent, L"AbsentChild", ERROR_FILE_NOT_FOUND, FALSE);
	// The failed opens must not create a key.
	HKEY absent_child = create_owned_child(parent, "AbsentChild");
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(absent_child));
	SetLastError(last_error_sentinel);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, RegOpenKeyA(parent, "MiXeDChild", NULL));
	TEST_CHECK_EQ(last_error_sentinel, GetLastError());
	SetLastError(last_error_sentinel);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, RegOpenKeyW(parent, L"MiXeDChild", NULL));
	TEST_CHECK_EQ(last_error_sentinel, GetLastError());
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(child_a));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(child_w));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(original_child));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(parent));
	printf("legacy registry open tests passed: %u guarded A/W opens and two argument errors\n", checked_opens);
	return 0;
}
