#include "test_assert.h"
#include <stdint.h>
#include <windows.h>

typedef LONG(WINAPI *add_ref_fn)(ULONG, HMODULE);
typedef int (*value_fn)(void);

static const char module_name[] = "restricted_loader_dependency.dll";
static const DWORD seed = 0x4321;

static void add_reference(add_ref_fn add_ref, ULONG flags, HMODULE module, uint32_t expected) {
	SetLastError(seed);
	LONG status = add_ref(flags, module);
	DWORD error = GetLastError();
	TEST_CHECK_EQ(expected, (uint32_t)status);
	TEST_CHECK_EQ(seed, error);
}

static void check_export(HMODULE module) {
	TEST_CHECK(GetModuleHandleA(module_name) == module);
	value_fn value = (value_fn)(ULONG_PTR)GetProcAddress(module, "dependency_value");
	TEST_CHECK(value != NULL);
	TEST_CHECK_EQ(23, value());
}

int main(void) {
	HMODULE native = GetModuleHandleA("ntdll.dll");
	TEST_CHECK(native != NULL);
	add_ref_fn add_ref = (add_ref_fn)(ULONG_PTR)GetProcAddress(native, "LdrAddRefDll");
	TEST_CHECK(add_ref != NULL);
	HMODULE self = GetModuleHandleA(NULL);
	TEST_CHECK(self != NULL);
	const ULONG flags[] = {0, 1, 2, 3, 0xffffffffu};
	for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
		add_reference(add_ref, flags[i], NULL, 0xc000000du);
		add_reference(add_ref, flags[i], self, 0);
	}

	TEST_CHECK(GetModuleHandleA(module_name) == NULL);
	const ULONG ordinary_flags[] = {0, 2};
	for (unsigned i = 0; i < sizeof(ordinary_flags) / sizeof(ordinary_flags[0]); ++i) {
		HMODULE module = LoadLibraryA(module_name);
		TEST_CHECK(module != NULL);
		add_reference(add_ref, ordinary_flags[i], module, 0);
		TEST_CHECK(FreeLibrary(module));
		check_export(module);
		TEST_CHECK(FreeLibrary(module));
		TEST_CHECK(GetModuleHandleA(module_name) == NULL);
	}

	HMODULE module = LoadLibraryA(module_name);
	TEST_CHECK(module != NULL);
	add_reference(add_ref, 0, module, 0);
	HMODULE acquired = NULL;
	TEST_CHECK(GetModuleHandleExA(0, module_name, &acquired));
	TEST_CHECK(acquired == module);
	for (unsigned i = 0; i < 2; ++i) {
		TEST_CHECK(FreeLibrary(module));
		check_export(module);
	}
	TEST_CHECK(FreeLibrary(module));
	TEST_CHECK(GetModuleHandleA(module_name) == NULL);

	module = LoadLibraryA(module_name);
	TEST_CHECK(module != NULL);
	add_reference(add_ref, 3, module, 0);
	add_reference(add_ref, 1, module, 0);
	add_reference(add_ref, 0, module, 0);
	add_reference(add_ref, 0xffffffffu, module, 0);
	for (unsigned i = 0; i < 4; ++i) {
		TEST_CHECK(FreeLibrary(module));
		check_export(module);
	}
	return 0;
}
