#include "test_assert.h"
#include <windows.h>

typedef int(WINAPI *attached_fn)(void);
typedef int(WINAPI *add_fn)(int, int);

static void check_missing_name(const char *name) {
	SetLastError(0x71);
	TEST_CHECK(LoadLibraryExA(name, NULL, 0) == NULL);
	TEST_CHECK_EQ(ERROR_MOD_NOT_FOUND, GetLastError());
}

int main(void) {
	check_missing_name(NULL);
	check_missing_name("");
	check_missing_name("   ");
	check_missing_name("synthetic_absent_load_module.dll");
	check_missing_name("kernel32.");
	check_missing_name("external_exports.");
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	TEST_CHECK(kernel != NULL);
	const char *names[] = {"kernel32.dll", "KERNEL32", "kernel32.dll ", "kernel32 ", "kernel32.dll."};
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
		SetLastError(0x71);
		HMODULE module = LoadLibraryExA(names[i], NULL, 0);
		TEST_CHECK(module == kernel);
		TEST_CHECK_EQ(0x71, GetLastError());
		TEST_CHECK(FreeLibrary(module));
	}
	SetLastError(0x71);
	HMODULE reserved = LoadLibraryExA("kernel32.dll", (HANDLE)(ULONG_PTR)1, 0);
	TEST_CHECK(reserved == kernel);
	TEST_CHECK_EQ(0x71, GetLastError());
	TEST_CHECK(FreeLibrary(reserved));
	HMODULE systemKernel = LoadLibraryExA("kernel32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	TEST_CHECK(systemKernel == kernel);
	TEST_CHECK(FreeLibrary(systemKernel));
	SetLastError(0x71);
	TEST_CHECK(LoadLibraryExA("external_exports.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) == NULL);
	TEST_CHECK_EQ(ERROR_MOD_NOT_FOUND, GetLastError());
	TEST_CHECK(LoadLibraryExW(L"external_exports.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) == NULL);
	TEST_CHECK_EQ(ERROR_MOD_NOT_FOUND, GetLastError());
	TEST_CHECK(GetModuleHandleA("external_exports.dll") == NULL);
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		const DWORD unsupported[] = {DONT_RESOLVE_DLL_REFERENCES, LOAD_IGNORE_CODE_AUTHZ_LEVEL, 0x80000000};
		for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
			SetLastError(0x71);
			TEST_CHECK(LoadLibraryExA("external_exports.dll", NULL, unsupported[i]) == NULL);
			TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
			TEST_CHECK(GetModuleHandleA("external_exports.dll") == NULL);
		}
	}
	HMODULE module = LoadLibraryExA("external_exports.dll", NULL, 0);
	TEST_CHECK_MSG(module != NULL, "LoadLibraryExA failed: %lu", GetLastError());
#ifdef _WIN64
	const char *attachedName = "was_attached";
	const char *addName = "add_numbers";
#else
	const char *attachedName = "was_attached@0";
	const char *addName = "add_numbers@8";
#endif
	attached_fn attached = (attached_fn)(ULONG_PTR)GetProcAddress(module, attachedName);
	add_fn add = (add_fn)(ULONG_PTR)GetProcAddress(module, addName);
	TEST_CHECK(attached && add);
	TEST_CHECK_EQ(1, attached());
	TEST_CHECK_EQ(42, add(19, 23));
	HMODULE wide = LoadLibraryExW(L"EXTERNAL_EXPORTS.DLL", NULL, 0);
	TEST_CHECK(wide == module);
	HMODULE systemLoaded = LoadLibraryExA("external_exports.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	TEST_CHECK(systemLoaded == module);
	TEST_CHECK(FreeLibrary(systemLoaded));
	HMODULE resourceLoaded =
		LoadLibraryExA("external_exports.dll", NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_SEARCH_SYSTEM32);
	TEST_CHECK(resourceLoaded == module);
	TEST_CHECK(FreeLibrary(resourceLoaded));
	char path[MAX_PATH];
	DWORD length = GetModuleFileNameA(module, path, sizeof(path));
	TEST_CHECK(length != 0 && length < sizeof(path));
	HMODULE byPath = LoadLibraryExA(path, NULL, 0);
	TEST_CHECK(byPath == module);
	TEST_CHECK(FreeLibrary(module));
	TEST_CHECK(FreeLibrary(wide));
	TEST_CHECK(GetModuleHandleA("external_exports.dll") == byPath);
	TEST_CHECK_EQ(1, attached());
	TEST_CHECK(FreeLibrary(byPath));
	TEST_CHECK(GetModuleHandleA("external_exports.dll") == NULL);
	return 0;
}
