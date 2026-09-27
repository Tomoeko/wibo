#include "test_assert.h"
#include <windows.h>

typedef int (*value_fn)(void);

struct search_case {
	DWORD flags;
	const char *name;
	BOOL user_directory;
	int result;
};

static void fixture_path(const char *base, const char *suffix, char *output, size_t capacity) {
	int length = snprintf(output, capacity, "%s\\%s", base, suffix);
	TEST_CHECK(length > 0 && (size_t)length < capacity);
}

static void check_case(const struct search_case *test, const char *base, BOOL wide) {
	char path[MAX_PATH * 2];
	if (test->user_directory) {
		fixture_path(base, "search-mode\\user", path, sizeof(path));
		TEST_CHECK(SetDllDirectoryA(path));
	} else {
		TEST_CHECK(SetDllDirectoryA(NULL));
	}
	if (!strcmp(test->name, "bare")) {
		strcpy(path, "kernel32.dll");
	} else if (!strcmp(test->name, "relative")) {
		strcpy(path, "SEARCH-MODE\\ROOT\\flags_priority.dll");
	} else {
		char suffix[MAX_PATH];
		int length = snprintf(suffix, sizeof(suffix), "search-mode\\root\\flags_%s.dll", test->name);
		TEST_CHECK(length > 0 && (size_t)length < sizeof(suffix));
		fixture_path(base, suffix, path, sizeof(path));
	}
	WCHAR wide_path[MAX_PATH * 2];
	TEST_CHECK(MultiByteToWideChar(CP_ACP, 0, path, -1, wide_path, MAX_PATH * 2));
	SetLastError(0x71);
	HMODULE module = wide ? LoadLibraryExW(wide_path, NULL, test->flags) : LoadLibraryExA(path, NULL, test->flags);
	DWORD error = GetLastError();
	TEST_CHECK_MSG((module != NULL) == (test->result >= 0), "case=%s flags=%lx wide=%d error=%lu", test->name,
				   (unsigned long)test->flags, wide, (unsigned long)error);
	if (test->result < 0) {
		TEST_CHECK_EQ(-test->result, error);
		return;
	}
	if (strcmp(test->name, "bare")) {
		const char *name = !strcmp(test->name, "forwarder") ? "forward_value" : "parent_value";
		value_fn value = (value_fn)(ULONG_PTR)GetProcAddress(module, name);
		TEST_CHECK(value != NULL);
		TEST_CHECK_EQ(test->result, value());
	}
	TEST_CHECK(FreeLibrary(module));
	const char *dependencies[] = {"dep_priority.dll",  "dep_fallback.dll", "dep_excluded.dll",	 "dep_environment.dll",
								  "dep_inherited.dll", "flags_child.dll",  "flags_forwarder.dll"};
	for (unsigned i = 0; i < sizeof(dependencies) / sizeof(dependencies[0]); ++i)
		TEST_CHECK(GetModuleHandleA(dependencies[i]) == NULL);
}

static void check_loaded_builtin(const char *base) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	char source[MAX_PATH * 2], shadow[MAX_PATH * 2], directory[MAX_PATH * 2];
	fixture_path(base, "dep_priority.dll", source, sizeof(source));
	fixture_path(base, "search-mode\\user\\kernel32.dll", shadow, sizeof(shadow));
	HANDLE input = CreateFileA(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(input != INVALID_HANDLE_VALUE);
	HANDLE output = CreateFileA(shadow, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(output != INVALID_HANDLE_VALUE);
	unsigned char buffer[4096];
	for (;;) {
		DWORD read = 0;
		TEST_CHECK(ReadFile(input, buffer, sizeof(buffer), &read, NULL));
		if (!read)
			break;
		DWORD written = 0;
		TEST_CHECK(WriteFile(output, buffer, read, &written, NULL));
		TEST_CHECK_EQ(read, written);
	}
	TEST_CHECK(CloseHandle(input));
	TEST_CHECK(CloseHandle(output));
	fixture_path(base, "search-mode\\user", directory, sizeof(directory));
	TEST_CHECK(SetDllDirectoryA(directory));
	const DWORD flags[] = {LOAD_LIBRARY_SEARCH_USER_DIRS, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
						   LOAD_WITH_ALTERED_SEARCH_PATH};
	for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
		HMODULE module = LoadLibraryExA("kernel32.dll", NULL, flags[i]);
		TEST_CHECK(module == kernel);
		TEST_CHECK(FreeLibrary(module));
		module = LoadLibraryExW(L"kernel32.dll", NULL, flags[i]);
		TEST_CHECK(module == kernel);
		TEST_CHECK(FreeLibrary(module));
	}
	TEST_CHECK(GetModuleHandleA("kernel32.dll") == kernel);
	TEST_CHECK(DeleteFileA(shadow));
	TEST_CHECK(SetDllDirectoryA(NULL));
}

int main(void) {
	char base[MAX_PATH], path[MAX_PATH * 2];
	DWORD length = GetModuleFileNameA(NULL, base, sizeof(base));
	TEST_CHECK(length && length < sizeof(base));
	char *separator = strrchr(base, '\\');
	TEST_CHECK(separator != NULL);
	*separator = 0;
	fixture_path(base, "search-mode\\current", path, sizeof(path));
	TEST_CHECK(SetCurrentDirectoryA(path));
	fixture_path(base, "search-mode\\environment", path, sizeof(path));
	TEST_CHECK(SetEnvironmentVariableA("PATH", path));
	const struct search_case cases[] = {
		{LOAD_WITH_ALTERED_SEARCH_PATH, "priority", FALSE, 11},
		{LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "priority", FALSE, 11},
		{0, "priority", FALSE, 22},
		{LOAD_WITH_ALTERED_SEARCH_PATH, "fallback", FALSE, 33},
		{LOAD_WITH_ALTERED_SEARCH_PATH, "fallback", TRUE, 44},
		{LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "fallback", FALSE, 22},
		{LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "fallback", TRUE, 22},
		{LOAD_WITH_ALTERED_SEARCH_PATH, "excluded", FALSE, 33},
		{LOAD_WITH_ALTERED_SEARCH_PATH, "excluded", TRUE, 55},
		{LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "excluded", FALSE, -ERROR_MOD_NOT_FOUND},
		{LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "excluded", TRUE, -ERROR_MOD_NOT_FOUND},
		{LOAD_WITH_ALTERED_SEARCH_PATH, "environment", FALSE, 55},
		{LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "environment", FALSE,
		 -ERROR_MOD_NOT_FOUND},
		{LOAD_WITH_ALTERED_SEARCH_PATH, "forwarder", FALSE, 22},
		{LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "forwarder", FALSE, 22},
		{LOAD_WITH_ALTERED_SEARCH_PATH, "forward_parent", FALSE, 11},
		{LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "forward_parent", FALSE, 11},
		{LOAD_WITH_ALTERED_SEARCH_PATH, "inherited", TRUE, 11},
		{LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "inherited", TRUE, 11},
		{LOAD_WITH_ALTERED_SEARCH_PATH, "inherited", FALSE, -ERROR_MOD_NOT_FOUND},
		{LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "inherited", FALSE, 11},
		{LOAD_WITH_ALTERED_SEARCH_PATH, "bare", FALSE, 0},
		{LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "bare", FALSE, -ERROR_INVALID_PARAMETER},
		{LOAD_WITH_ALTERED_SEARCH_PATH | LOAD_LIBRARY_SEARCH_SYSTEM32, "priority", FALSE, -ERROR_INVALID_PARAMETER},
		{LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR, "priority", FALSE, 11},
		{LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR, "fallback", FALSE, -ERROR_MOD_NOT_FOUND},
		{LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "priority", FALSE, 22},
		{LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "inherited", FALSE, 22},
		{LOAD_LIBRARY_SEARCH_APPLICATION_DIR, "relative", FALSE, 22},
	};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		check_case(&cases[i], base, FALSE);
		check_case(&cases[i], base, TRUE);
	}
	check_loaded_builtin(base);
	return 0;
}
