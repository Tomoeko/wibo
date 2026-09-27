#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "test_assert.h"

static const DWORD kError = 0x13579bdf;

typedef struct {
	uint64_t before;
	union {
		WCHAR wide[128];
		char narrow[256];
	} value;
	uint64_t after;
} StringBuffer;

static void check_case(HKEY key, int ansi, int extended, DWORD index, DWORD capacity, DWORD class_capacity,
					   int class_mode, int reserved, LSTATUS expected_status, const FILETIME *expected_time) {
	StringBuffer name, key_class;
	memset(&name, 0xa5, sizeof(name));
	memset(&key_class, 0xa5, sizeof(key_class));
	StringBuffer original;
	memcpy(&original, &name, sizeof(original));
	DWORD length = capacity, class_length = class_capacity, reserved_value = 0x11223344;
	FILETIME time = {0x11223344, 0x55667788};
	SetLastError(kError);
	LSTATUS status;
	if (extended && ansi)
		status = RegEnumKeyExA(key, index, name.value.narrow, &length, reserved ? &reserved_value : NULL,
							   class_mode == 0 ? NULL : key_class.value.narrow, class_mode == 2 ? NULL : &class_length,
							   &time);
	else if (extended)
		status =
			RegEnumKeyExW(key, index, name.value.wide, &length, reserved ? &reserved_value : NULL,
						  class_mode == 0 ? NULL : key_class.value.wide, class_mode == 2 ? NULL : &class_length, &time);
	else if (ansi)
		status = RegEnumKeyA(key, index, name.value.narrow, capacity);
	else
		status = RegEnumKeyW(key, index, name.value.wide, capacity);
	TEST_CHECK_EQ(expected_status, status);
	TEST_CHECK_EQ(kError, GetLastError());
	TEST_CHECK_U64_EQ(original.before, name.before);
	TEST_CHECK_U64_EQ(original.after, name.after);
	TEST_CHECK_U64_EQ(original.before, key_class.before);
	TEST_CHECK_U64_EQ(original.after, key_class.after);
	if (!status) {
		if (ansi)
			TEST_CHECK_STR_EQ("MiXeD", name.value.narrow);
		else
			TEST_CHECK_EQ(0, wcscmp(L"MiXeD", name.value.wide));
		TEST_CHECK_EQ(extended ? 5 : capacity, length);
		TEST_CHECK_EQ(extended && class_mode != 2 ? 5 : class_capacity, class_length);
		if (extended && class_mode == 1) {
			if (ansi)
				TEST_CHECK_STR_EQ("Class", key_class.value.narrow);
			else
				TEST_CHECK_EQ(0, wcscmp(L"Class", key_class.value.wide));
		} else
			TEST_CHECK_EQ(0, memcmp(&original.value, &key_class.value, sizeof(original.value)));
	} else {
		TEST_CHECK_EQ(capacity, length);
		TEST_CHECK_EQ(class_capacity, class_length);
		TEST_CHECK_EQ(0, memcmp(&original.value, &name.value, sizeof(original.value)));
		TEST_CHECK_EQ(0, memcmp(&original.value, &key_class.value, sizeof(original.value)));
	}
	if (extended && expected_time && (status == ERROR_SUCCESS || status == ERROR_MORE_DATA)) {
		TEST_CHECK_EQ(expected_time->dwLowDateTime, time.dwLowDateTime);
		TEST_CHECK_EQ(expected_time->dwHighDateTime, time.dwHighDateTime);
	} else {
		TEST_CHECK_EQ(0x11223344, time.dwLowDateTime);
		TEST_CHECK_EQ(0x55667788, time.dwHighDateTime);
	}
}

static HKEY create(HKEY parent, const WCHAR *name, WCHAR *key_class) {
	HKEY key = NULL;
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCreateKeyExW(parent, name, 0, key_class, 0, KEY_ALL_ACCESS, NULL, &key, NULL));
	return key;
}

static void test_matrix(HKEY parent) {
	HKEY child = create(parent, L"MiXeD", L"Class");
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(child));
	WCHAR name[64], cls[64];
	DWORD length = 64, class_length = 64;
	FILETIME time;
	TEST_CHECK_EQ(ERROR_SUCCESS, RegEnumKeyExW(parent, 0, name, &length, NULL, cls, &class_length, &time));
	TEST_CHECK(time.dwLowDateTime || time.dwHighDateTime);
	const DWORD capacities[] = {0, 1, 5, 6, 64};
	for (int ansi = 0; ansi <= 1; ++ansi) {
		for (int extended = 0; extended <= 1; ++extended) {
			for (size_t i = 0; i < sizeof(capacities) / sizeof(*capacities); ++i)
				check_case(parent, ansi, extended, 0, capacities[i], 64, 1, 0,
						   capacities[i] <= 5 ? ERROR_MORE_DATA : ERROR_SUCCESS, &time);
			check_case(parent, ansi, extended, 1, 64, 64, 1, 0, ERROR_NO_MORE_ITEMS, &time);
			check_case(parent, ansi, extended, 1, 0, 0, 1, 0, ERROR_NO_MORE_ITEMS, &time);
		}
		for (size_t i = 0; i < sizeof(capacities) / sizeof(*capacities); ++i)
			check_case(parent, ansi, 1, 0, 64, capacities[i], 1, 0,
					   capacities[i] <= 5 ? ERROR_MORE_DATA : ERROR_SUCCESS, &time);
		check_case(parent, ansi, 1, 0, 64, 0, 0, 0, ERROR_SUCCESS, &time);
		check_case(parent, ansi, 1, 0, 64, 64, 2, 0, ERROR_SUCCESS, &time);
		check_case(parent, ansi, 1, 0, 64, 64, 1, 1, ERROR_INVALID_PARAMETER, &time);
		check_case(NULL, ansi, 1, 0, 64, 64, 1, 0, ERROR_INVALID_HANDLE, &time);
		check_case(NULL, ansi, 1, 0, 64, 64, 1, 1, ERROR_INVALID_PARAMETER, &time);
	}
	HKEY query = NULL;
	TEST_CHECK_EQ(ERROR_SUCCESS, RegOpenKeyExW(parent, L"", 0, KEY_QUERY_VALUE, &query));
	check_case(query, 0, 1, 0, 64, 64, 1, 0, ERROR_ACCESS_DENIED, &time);
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(query));
	HKEY reopened = NULL;
	TEST_CHECK_EQ(ERROR_SUCCESS,
				  RegCreateKeyExW(parent, L"", 0, L"Ignored", 0, KEY_ENUMERATE_SUB_KEYS, NULL, &reopened, NULL));
	TEST_CHECK(reopened != parent);
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(reopened));
	check_case(parent, 0, 1, 0, 64, 64, 1, 0, ERROR_SUCCESS, &time);
	HANDLE duplicate = NULL;
	TEST_CHECK(
		DuplicateHandle(GetCurrentProcess(), parent, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
	SetLastError(kError);
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(parent));
	TEST_CHECK_EQ(kError, GetLastError());
	check_case(parent, 0, 1, 0, 64, 64, 1, 0, ERROR_INVALID_HANDLE, &time);
	SetLastError(kError);
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, RegCloseKey(parent));
	TEST_CHECK_EQ(kError, GetLastError());
	check_case((HKEY)duplicate, 0, 1, 0, 64, 64, 1, 0, ERROR_SUCCESS, &time);
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey((HKEY)duplicate));
}

static void test_components(HKEY parent) {
	HKEY child = create(parent, L"x/y", L"SlashClass");
	RegCloseKey(child);
	child = create(parent, L"Outer\\Inner", L"LeafClass");
	RegCloseKey(child);
	unsigned seen = 0;
	for (DWORD i = 0; i < 2; ++i) {
		WCHAR name[64], cls[64];
		DWORD n = 64, c = 64;
		FILETIME time;
		TEST_CHECK_EQ(ERROR_SUCCESS, RegEnumKeyExW(parent, i, name, &n, NULL, cls, &c, &time));
		TEST_CHECK(time.dwLowDateTime || time.dwHighDateTime);
		if (wcscmp(name, L"x/y") == 0) {
			TEST_CHECK_EQ(0, wcscmp(cls, L"SlashClass"));
			seen |= 1;
		} else {
			TEST_CHECK_EQ(0, wcscmp(name, L"Outer"));
			TEST_CHECK_EQ(0, wcscmp(cls, L"LeafClass"));
			seen |= 2;
		}
	}
	TEST_CHECK_EQ(3, seen);
	WCHAR name[64];
	DWORD n = 64;
	TEST_CHECK_EQ(ERROR_NO_MORE_ITEMS, RegEnumKeyExW(parent, 2, name, &n, NULL, NULL, NULL, NULL));
	HKEY outer = NULL;
	TEST_CHECK_EQ(ERROR_SUCCESS, RegOpenKeyExW(parent, L"outer", 0, KEY_ENUMERATE_SUB_KEYS, &outer));
	n = 64;
	TEST_CHECK_EQ(ERROR_SUCCESS, RegEnumKeyExW(outer, 0, name, &n, NULL, NULL, NULL, NULL));
	TEST_CHECK_EQ(0, wcscmp(name, L"Inner"));
	RegCloseKey(outer);
}

int main(int argc, char **argv) {
	int cleanup = argc == 2 && strcmp(argv[1], "--native-cleanup") == 0;
	TEST_CHECK(argc == 1 || cleanup);
	WCHAR path[128];
	swprintf(path, 128, L"Software\\wibo_registry_enum_test_%lu", (unsigned long)GetCurrentProcessId());
	HKEY root = create(HKEY_CURRENT_USER, path, NULL);
	HKEY matrix = create(root, L"Matrix", NULL);
	test_matrix(matrix);
	HKEY components = create(root, L"Components", NULL);
	test_components(components);
	RegCloseKey(components);
	SetLastError(kError);
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(HKEY_CURRENT_USER));
	TEST_CHECK_EQ(kError, GetLastError());
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, RegCloseKey(NULL));
	TEST_CHECK_EQ(kError, GetLastError());
	RegCloseKey(root);
	if (cleanup)
		TEST_CHECK_EQ(ERROR_SUCCESS, RegDeleteTreeW(HKEY_CURRENT_USER, path));
	return 0;
}
