#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#include "test_assert.h"

int main(void) {
    typedef int(__stdcall *add_numbers_fn)(int, int);
    typedef int(__stdcall *was_attached_fn)(void);
    typedef HMODULE(__stdcall *observed_main_module_fn)(void);

    HMODULE initial_main = GetModuleHandleW(NULL);
    TEST_CHECK(initial_main != NULL);

    HMODULE mod = LoadLibraryA("external_exports.dll");
    TEST_CHECK_MSG(mod != NULL, "LoadLibraryA failed: %lu", (unsigned long)GetLastError());

#ifdef _WIN64
    const char *add_name = "add_numbers";
    const char *attached_name = "was_attached";
    const char *main_name = "observed_main_module";
#else
    const char *add_name = "add_numbers@8";
    const char *attached_name = "was_attached@0";
    const char *main_name = "observed_main_module@0";
#endif
    FARPROC raw_add_numbers = GetProcAddress(mod, add_name);
    FARPROC raw_was_attached = GetProcAddress(mod, attached_name);
    FARPROC raw_observed_main = GetProcAddress(mod, main_name);
    TEST_CHECK_MSG(raw_add_numbers != NULL, "GetProcAddress(%s) failed: %lu", add_name,
                   (unsigned long)GetLastError());
    TEST_CHECK_MSG(raw_was_attached != NULL, "GetProcAddress(%s) failed: %lu", attached_name,
                   (unsigned long)GetLastError());
    TEST_CHECK_MSG(raw_observed_main != NULL, "GetProcAddress(%s) failed: %lu", main_name,
                   (unsigned long)GetLastError());

    add_numbers_fn add_numbers = (add_numbers_fn)(uintptr_t)raw_add_numbers;
    was_attached_fn was_attached = (was_attached_fn)(uintptr_t)raw_was_attached;
    observed_main_module_fn observed_main = (observed_main_module_fn)(uintptr_t)raw_observed_main;

    int sum = add_numbers(2, 40);
    int attached = was_attached();
    HMODULE observed_main_module = observed_main();

    TEST_CHECK_EQ(42, sum);
    TEST_CHECK_EQ(1, attached);
    TEST_CHECK_EQ(initial_main, observed_main_module);

    TEST_CHECK_MSG(FreeLibrary(mod) != 0, "FreeLibrary failed: %lu", (unsigned long)GetLastError());

    printf("external_exports: sum=%d attached=%d\n", sum, attached);
    return EXIT_SUCCESS;
}
