#include "test_assert.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

static int env_name_equals(const char *entry, const char *name) {
	size_t i = 0;
	while (name[i] != '\0') {
		if (entry[i] == '\0' || entry[i] == '=') {
			return 0;
		}
		if (tolower((unsigned char)entry[i]) != tolower((unsigned char)name[i])) {
			return 0;
		}
		i++;
	}
	return entry[i] == '=';
}

static const WCHAR *wide_block_value(const WCHAR *block, const WCHAR *name) {
	size_t length = wcslen(name);
	for (const WCHAR *entry = block; *entry; entry += wcslen(entry) + 1) {
		if (!wcsncmp(entry, name, length) && entry[length] == '=')
			return entry + length + 1;
	}
	return NULL;
}

static void test_wide_environment(void) {
	static const WCHAR name[] = L"WIBO_ENV_\x4e2d";
	static const WCHAR lower_name[] = L"wibo_env_\x4e2d";
	static const WCHAR value[] = {'a', 0x00e9, 0x4e2d, 0xd83d, 0xde00, 0};
	static const WCHAR replacement[] = {0x03a9, 0xd83d, 0xde03, 0};
	const DWORD required = (DWORD)(sizeof(value) / sizeof(value[0]));
	const DWORD sentinel_error = 0x12345678;
	WCHAR buffer[16];
	SetLastError(sentinel_error);
	TEST_CHECK(SetEnvironmentVariableW(name, value));
	TEST_CHECK_EQ(sentinel_error, GetLastError());
	TEST_CHECK_EQ(required, GetEnvironmentVariableW(lower_name, NULL, 0));
	TEST_CHECK_EQ(sentinel_error, GetLastError());
	for (unsigned i = 0; i < sizeof(buffer) / sizeof(buffer[0]); ++i)
		buffer[i] = '!';
	TEST_CHECK_EQ(required, GetEnvironmentVariableW(name, buffer, 0));
	TEST_CHECK_EQ('!', buffer[0]);
	TEST_CHECK_EQ(required, GetEnvironmentVariableW(name, buffer, required - 1));
	// Only the storage outside a short buffer has a defined invariant.
	TEST_CHECK_EQ('!', buffer[required - 1]);
	TEST_CHECK_EQ(required - 1, GetEnvironmentVariableW(lower_name, buffer, required));
	TEST_CHECK_EQ(0, memcmp(buffer, value, sizeof(value)));
	TEST_CHECK_EQ('!', buffer[required]);
	TEST_CHECK_EQ(sentinel_error, GetLastError());

	LPWCH snapshot = GetEnvironmentStringsW();
	TEST_CHECK(snapshot != NULL);
	const WCHAR *original_value = wide_block_value(snapshot, name);
	TEST_CHECK(original_value != NULL);
	TEST_CHECK_EQ(0, wcscmp(value, original_value));
	TEST_CHECK(SetEnvironmentVariableW(name, replacement));
	TEST_CHECK_EQ(0, wcscmp(value, original_value));
	LPWCH current = GetEnvironmentStringsW();
	TEST_CHECK(current != NULL);
	const WCHAR *current_value = wide_block_value(current, name);
	TEST_CHECK(current_value != NULL);
	TEST_CHECK_EQ(0, wcscmp(replacement, current_value));
	TEST_CHECK(FreeEnvironmentStringsW(current));
	TEST_CHECK(FreeEnvironmentStringsW(snapshot));

	TEST_CHECK(SetEnvironmentVariableW(name, L""));
	SetLastError(sentinel_error);
	TEST_CHECK_EQ(1, GetEnvironmentVariableW(name, NULL, 0));
	buffer[0] = '!';
	TEST_CHECK_EQ(0, GetEnvironmentVariableW(name, buffer, 1));
	TEST_CHECK_EQ(0, buffer[0]);
	TEST_CHECK_EQ(sentinel_error, GetLastError());
	TEST_CHECK(SetEnvironmentVariableW(lower_name, NULL));
	SetLastError(sentinel_error);
	TEST_CHECK_EQ(0, GetEnvironmentVariableW(name, buffer, 16));
	TEST_CHECK_EQ(ERROR_ENVVAR_NOT_FOUND, GetLastError());
	current = GetEnvironmentStringsW();
	TEST_CHECK(current != NULL);
	TEST_CHECK(wide_block_value(current, name) == NULL);
	TEST_CHECK(FreeEnvironmentStringsW(current));
	TEST_CHECK(!SetEnvironmentVariableW(L"WIBO_ENV_INVALID=NAME", L"value"));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
}

int main(void) {
	test_wide_environment();
	// Get the environment block
	LPCH env = GetEnvironmentStringsA();
	TEST_CHECK(env != NULL);

	// Parse the block: NULL-terminated strings, ending with a double NULL
	char *p = env;
	int foundPath = 0;
	int foundTmp = 0;
	int foundTemp = 0;
	while (*p != '\0') {
		if (env_name_equals(p, "PATH")) {
			foundPath = 1;
			char *pathValue = p + 5;
			// In Wibo, converted PATHs are Z:\...;Z:\...
			// So they must contain at least one ';'
			// They should NOT contain ':' as a delimiter, only within drive letters like Z:
			// This check is a simple heuristic.
			if (strchr(pathValue, ';') != NULL) {
				// Success: PATH converted to semicolon-delimited
			} else {
				TEST_CHECK_MSG(0, "PATH does not contain ';', value: %s", pathValue);
			}
		} else if (env_name_equals(p, "TMP")) {
			foundTmp = 1;
		} else if (env_name_equals(p, "TEMP")) {
			foundTemp = 1;
		}
		p += strlen(p) + 1;
	}

	TEST_CHECK(foundPath);
	TEST_CHECK(foundTmp);
	TEST_CHECK(foundTemp);

	FreeEnvironmentStringsA(env);

	char buffer[MAX_PATH];
	DWORD len = GetEnvironmentVariableA("TMP", buffer, sizeof(buffer));
	TEST_CHECK(len > 0 && len < sizeof(buffer));
	TEST_CHECK_MSG(strchr(buffer, '\\') != NULL, "TMP should be a Windows path, got: %s", buffer);

	len = GetEnvironmentVariableA("TEMP", buffer, sizeof(buffer));
	TEST_CHECK(len > 0 && len < sizeof(buffer));
	TEST_CHECK_MSG(strchr(buffer, '\\') != NULL, "TEMP should be a Windows path, got: %s", buffer);

	TEST_CHECK(SetEnvironmentVariableA("TMP", NULL));
	SetLastError(ERROR_SUCCESS);
	TEST_CHECK_EQ(0, GetEnvironmentVariableA("TMP", buffer, sizeof(buffer)));
	TEST_CHECK_EQ(ERROR_ENVVAR_NOT_FOUND, GetLastError());

	env = GetEnvironmentStringsA();
	TEST_CHECK(env != NULL);
	p = env;
	foundTmp = 0;
	while (*p != '\0') {
		if (env_name_equals(p, "TMP")) {
			foundTmp = 1;
		}
		p += strlen(p) + 1;
	}
	TEST_CHECK(!foundTmp);
	FreeEnvironmentStringsA(env);

	return EXIT_SUCCESS;
}
