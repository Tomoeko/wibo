#include "test_assert.h"
#include <windows.h>

typedef int (*value_fn)(void);

static int detach_count[2];
static int detach_value[2];
static int detach_visible[2];
static BOOL retain_cycle_self;
static HMODULE retained_cycle;

__declspec(dllexport) void lifetime_detach_record(int member, int value, int visible) {
	TEST_CHECK(member >= 0 && member < 2);
	++detach_count[member];
	detach_value[member] = value;
	detach_visible[member] = visible;
	if (member == 0 && retain_cycle_self)
		TEST_CHECK(GetModuleHandleExA(0, "module_lifetime_cycle_a.dll", &retained_cycle));
}

static value_fn value_export(HMODULE module, const char *name) {
	value_fn function = (value_fn)(ULONG_PTR)GetProcAddress(module, name);
	TEST_CHECK(function != NULL);
	return function;
}

static void fixture_path(const char *name, char *path) {
	DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
	TEST_CHECK(length != 0 && length < MAX_PATH);
	char *separator = strrchr(path, '\\');
	TEST_CHECK(separator != NULL);
	TEST_CHECK((size_t)(separator - path) + 1 + strlen(name) < MAX_PATH);
	strcpy(separator + 1, name);
}

int main(void) {
	TEST_CHECK(GetModuleHandleA("restricted_loader_dependency.dll") == NULL);
	HMODULE forwarder = LoadLibraryA("system_loader_forwarder.dll");
	TEST_CHECK(forwarder != NULL);
	value_fn first = value_export(forwarder, "forward_value");
	TEST_CHECK_EQ(23, first());
	for (unsigned i = 0; i < 8; ++i)
		TEST_CHECK(value_export(forwarder, "forward_value") == first);
	TEST_CHECK(FreeLibrary(forwarder));
	TEST_CHECK(GetModuleHandleA("restricted_loader_dependency.dll") == NULL);

	forwarder = LoadLibraryA("system_loader_forwarder.dll");
	TEST_CHECK(forwarder != NULL);
	HMODULE parent = LoadLibraryA("restricted_loader_forward_parent.dll");
	TEST_CHECK(parent != NULL);
	TEST_CHECK_EQ(24, value_export(parent, "parent_value")());
	TEST_CHECK(GetModuleHandleA("restricted_loader_dependency.dll") != NULL);
	TEST_CHECK(FreeLibrary(parent));
	TEST_CHECK(GetModuleHandleA("system_loader_forwarder.dll") == forwarder);
	TEST_CHECK(GetModuleHandleA("restricted_loader_dependency.dll") == NULL);
	TEST_CHECK(FreeLibrary(forwarder));

	forwarder = LoadLibraryA("module_forward_failure.dll");
	TEST_CHECK(forwarder != NULL);
	const char *missing_exports[] = {"missing_module", "missing_export"};
	for (unsigned i = 0; i < sizeof(missing_exports) / sizeof(missing_exports[0]); ++i) {
		SetLastError(0x71);
		TEST_CHECK(GetProcAddress(forwarder, missing_exports[i]) == NULL);
		TEST_CHECK_EQ(ERROR_PROC_NOT_FOUND, GetLastError());
	}
	TEST_CHECK(FreeLibrary(forwarder));
	TEST_CHECK(GetModuleHandleA("restricted_loader_dependency.dll") == NULL);

	char path[MAX_PATH];
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		HMODULE available = LoadLibraryA("restricted_loader_dependency.dll");
		TEST_CHECK(available != NULL);
		fixture_path("module_lifetime_failure.dll", path);
		for (unsigned i = 0; i < 2; ++i) {
			SetLastError(0x71);
			TEST_CHECK(LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) == NULL);
			TEST_CHECK_EQ(ERROR_MOD_NOT_FOUND, GetLastError());
			TEST_CHECK(GetModuleHandleA("module_lifetime_failure.dll") == NULL);
			TEST_CHECK(GetModuleHandleA("restricted_loader_dependency.dll") == available);
		}
		TEST_CHECK(FreeLibrary(available));
		TEST_CHECK(GetModuleHandleA("restricted_loader_dependency.dll") == NULL);
	}

	HMODULE cycle = LoadLibraryA("module_lifetime_cycle_a.dll");
	TEST_CHECK(cycle != NULL);
	TEST_CHECK(GetModuleHandleA("module_lifetime_cycle_b.dll") != NULL);
	TEST_CHECK_EQ(38, value_export(cycle, "cycle_value_a")());
	TEST_CHECK(FreeLibrary(cycle));
	TEST_CHECK_EQ(1, detach_count[0]);
	TEST_CHECK_EQ(1, detach_count[1]);
	TEST_CHECK_EQ(37, detach_value[0]);
	TEST_CHECK_EQ(38, detach_value[1]);
	TEST_CHECK_EQ(1, detach_visible[0]);
	TEST_CHECK_EQ(1, detach_visible[1]);
	TEST_CHECK(GetModuleHandleA("module_lifetime_cycle_a.dll") == NULL);
	TEST_CHECK(GetModuleHandleA("module_lifetime_cycle_b.dll") == NULL);

	retain_cycle_self = TRUE;
	cycle = LoadLibraryA("module_lifetime_cycle_a.dll");
	TEST_CHECK(cycle != NULL);
	TEST_CHECK(FreeLibrary(cycle));
	retain_cycle_self = FALSE;
	TEST_CHECK(retained_cycle == cycle);
	TEST_CHECK(GetModuleHandleA("module_lifetime_cycle_a.dll") == cycle);
	TEST_CHECK(GetModuleHandleA("module_lifetime_cycle_b.dll") == NULL);
	TEST_CHECK_EQ(2, detach_count[0]);
	TEST_CHECK_EQ(2, detach_count[1]);
	TEST_CHECK(FreeLibrary(retained_cycle));
	TEST_CHECK(GetModuleHandleA("module_lifetime_cycle_a.dll") == NULL);
	TEST_CHECK_EQ(2, detach_count[0]);

	TEST_CHECK(SetEnvironmentVariableA("WIBO_LIFETIME_REJECT_ATTACH", "1"));
	SetLastError(0x71);
	TEST_CHECK(LoadLibraryA("module_lifetime_cycle_a.dll") == NULL);
	TEST_CHECK_EQ(ERROR_DLL_INIT_FAILED, GetLastError());
	TEST_CHECK_EQ(3, detach_count[0]);
	TEST_CHECK_EQ(3, detach_count[1]);
	TEST_CHECK_EQ(37, detach_value[0]);
	TEST_CHECK_EQ(38, detach_value[1]);
	TEST_CHECK(GetModuleHandleA("module_lifetime_cycle_a.dll") == NULL);
	TEST_CHECK(GetModuleHandleA("module_lifetime_cycle_b.dll") == NULL);
	TEST_CHECK(SetEnvironmentVariableA("WIBO_LIFETIME_REJECT_ATTACH", NULL));

	for (unsigned mode = 0; mode < 3; ++mode) {
		HMODULE retained = NULL;
		int observations[3] = {0};
		HMODULE module = LoadLibraryA("module_detach_reference.dll");
		TEST_CHECK(module != NULL);
		typedef void (*configure_fn)(int *, HMODULE *, BOOL);
		configure_fn configure = (configure_fn)(ULONG_PTR)GetProcAddress(module, "detach_configure");
		TEST_CHECK(configure != NULL);
		configure(observations, &retained, mode == 1);
		TEST_CHECK(FreeLibrary(module));
		TEST_CHECK(retained == module);
		TEST_CHECK(GetModuleHandleA("module_detach_reference.dll") == retained);
		TEST_CHECK_EQ(1, observations[0]);
		TEST_CHECK_EQ(1, observations[1]);
		TEST_CHECK_EQ(1, observations[2]);
		if (mode == 2) {
			HMODULE reloaded = LoadLibraryA("module_detach_reference.dll");
			TEST_CHECK(reloaded == retained);
			TEST_CHECK_EQ(2, observations[0]);
			TEST_CHECK(FreeLibrary(reloaded));
		}
		TEST_CHECK(FreeLibrary(retained));
		TEST_CHECK(GetModuleHandleA("module_detach_reference.dll") == NULL);
		TEST_CHECK_EQ(mode == 2 ? 2 : 1, observations[1]);
	}

	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		HMODULE builtin = GetModuleHandleA("version.dll");
		TEST_CHECK(builtin != NULL);
		FARPROC original = GetProcAddress(builtin, "GetFileVersionInfoSizeW");
		TEST_CHECK(original != NULL);
		fixture_path("failed-module\\version.dll", path);
		SetLastError(0x71);
		TEST_CHECK(LoadLibraryA(path) == NULL);
		TEST_CHECK_EQ(ERROR_DLL_INIT_FAILED, GetLastError());
		TEST_CHECK(GetModuleHandleA("version.dll") == builtin);
		TEST_CHECK(GetProcAddress(builtin, "GetFileVersionInfoSizeW") == original);

		cycle = LoadLibraryA("module_forward_cycle_a.dll");
		TEST_CHECK(cycle != NULL);
		SetLastError(0x71);
		TEST_CHECK(GetProcAddress(cycle, "cycle_value") == NULL);
		TEST_CHECK_EQ(ERROR_PROC_NOT_FOUND, GetLastError());
		TEST_CHECK(FreeLibrary(cycle));
		TEST_CHECK(GetModuleHandleA("module_forward_cycle_a.dll") == NULL);
		TEST_CHECK(GetModuleHandleA("module_forward_cycle_b.dll") == NULL);
	}
	return 0;
}
