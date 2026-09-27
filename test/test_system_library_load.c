#include "test_assert.h"
#include <windows.h>

typedef int (*value_fn)(void);

static void fixture_path(const char *name, char *path) {
	DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
	TEST_CHECK(length != 0 && length < MAX_PATH);
	char *separator = strrchr(path, '\\');
	TEST_CHECK(separator != NULL);
	TEST_CHECK((size_t)(separator - path) + 1 + strlen(name) < MAX_PATH);
	strcpy(separator + 1, name);
}

static value_fn value_export(HMODULE module, const char *name) {
	value_fn function = (value_fn)(ULONG_PTR)GetProcAddress(module, name);
	TEST_CHECK(function != NULL);
	return function;
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	SetLastError(0x71);
	HMODULE systemKernel = LoadLibraryExA("kernel32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	TEST_CHECK(systemKernel == kernel);
	TEST_CHECK_EQ(0x71, GetLastError());
	TEST_CHECK(FreeLibrary(systemKernel));

	if (getenv("WIBO_SYSTEM_FIXTURE")) {
		HMODULE parent = LoadLibraryExA("system_loader_parent.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		TEST_CHECK_MSG(parent != NULL, "System directory load failed: %lu", GetLastError());
		TEST_CHECK_EQ(24, value_export(parent, "parent_value")());
		char path[MAX_PATH];
		DWORD length = GetModuleFileNameA(parent, path, sizeof(path));
		TEST_CHECK(length != 0 && length < sizeof(path));
		TEST_CHECK(_strnicmp(path, "C:\\Windows\\System32\\", 20) == 0);
		HMODULE wide = LoadLibraryExW(L"system_loader_parent.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		TEST_CHECK(wide == parent);
		TEST_CHECK(FreeLibrary(wide));
		TEST_CHECK(FreeLibrary(parent));
		TEST_CHECK(GetModuleHandleA("system_loader_dependency.dll") == NULL);
		fixture_path("system_loader_failure.dll", path);
		for (unsigned i = 0; i < 2; ++i) {
			SetLastError(0x71);
			TEST_CHECK(LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) == NULL);
			TEST_CHECK_EQ(ERROR_MOD_NOT_FOUND, GetLastError());
			TEST_CHECK(GetModuleHandleA("system_loader_failure.dll") == NULL);
			TEST_CHECK(GetModuleHandleA("system_loader_dependency.dll") == NULL);
		}
	}

	char path[MAX_PATH];
	fixture_path("restricted_loader_parent.dll", path);
	for (unsigned i = 0; i < 2; ++i) {
		SetLastError(0x71);
		TEST_CHECK(LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) == NULL);
		TEST_CHECK_EQ(ERROR_MOD_NOT_FOUND, GetLastError());
		TEST_CHECK(GetModuleHandleA("restricted_loader_parent.dll") == NULL);
		TEST_CHECK(GetModuleHandleA("restricted_loader_dependency.dll") == NULL);
	}

	fixture_path("system_loader_forwarder.dll", path);
	HMODULE forwarder = LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	TEST_CHECK(forwarder != NULL);
	TEST_CHECK(GetModuleHandleA("restricted_loader_dependency.dll") == NULL);
	fixture_path("restricted_loader_forward_parent.dll", path);
	HMODULE forwardingParent = LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	TEST_CHECK(forwardingParent != NULL);
	TEST_CHECK(GetModuleHandleA("restricted_loader_dependency.dll") == NULL);
	TEST_CHECK(FreeLibrary(forwardingParent));
	TEST_CHECK_EQ(23, value_export(forwarder, "forward_value")());
	TEST_CHECK(GetModuleHandleA("restricted_loader_dependency.dll") != NULL);

	fixture_path("restricted_loader_parent.dll", path);
	HMODULE parent = LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	TEST_CHECK(parent != NULL);
	TEST_CHECK_EQ(24, value_export(parent, "parent_value")());
	TEST_CHECK(FreeLibrary(parent));
	TEST_CHECK(FreeLibrary(forwarder));
	return 0;
}
