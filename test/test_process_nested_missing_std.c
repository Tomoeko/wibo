#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "test_assert.h"

static const DWORD selectors[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};

static int launch(const char *phase) {
    char image[MAX_PATH], command[MAX_PATH + 40];
    const DWORD length = GetModuleFileNameA(NULL, image, sizeof(image));
    if (!length || length >= sizeof(image))
        return 10;
    const int count = snprintf(command, sizeof(command), "\"%s\" %s", image, phase);
    if (count <= 0 || (size_t)count >= sizeof(command))
        return 11;
    STARTUPINFOA startup = {0};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {0};
    if (!CreateProcessA(image, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        const DWORD error = GetLastError();
        return error ? (int)error : 12;
    }
    const DWORD waited = WaitForSingleObject(process.hProcess, 10000);
    DWORD result = 13;
    if (waited == WAIT_OBJECT_0 && !GetExitCodeProcess(process.hProcess, &result))
        result = 14;
    if (waited != WAIT_OBJECT_0)
        result = 15;
    const BOOL threadClosed = CloseHandle(process.hThread);
    const BOOL processClosed = CloseHandle(process.hProcess);
    return threadClosed && processClosed ? (int)result : 16;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "leaf") == 0)
        return 0;
    if (argc == 2 && strcmp(argv[1], "middle") == 0) {
        if (getenv("WIBO_FIXTURE_RUNTIME"))
            for (unsigned index = 0; index < 3; ++index)
                if (GetStdHandle(selectors[index]) != NULL)
                    return 17;
        return launch("leaf");
    }
    TEST_CHECK_EQ(1, argc);
    HANDLE original[3];
    for (unsigned index = 0; index < 3; ++index) {
        original[index] = GetStdHandle(selectors[index]);
        TEST_CHECK(SetStdHandle(selectors[index], NULL));
    }
    const int result = launch("middle");
    for (unsigned index = 0; index < 3; ++index)
        TEST_CHECK(SetStdHandle(selectors[index], original[index]));
    TEST_CHECK_EQ(0, result);
    return 0;
}
