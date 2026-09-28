#include <windows.h>

#include "test_assert.h"

int main(void) {
    const DWORD sentinel = 0x23456789;
    SetLastError(sentinel);
    const LANGID language = GetUserDefaultUILanguage();
    const DWORD error = GetLastError();
    printf("language=%04x error=%lu\n", language, (unsigned long)error);
    const char *mode = getenv("WIBO_FIXTURE_UI_LANGUAGE_RESPONSE");
    if (!mode) {
        TEST_CHECK(language != 0);
        TEST_CHECK_EQ(sentinel, error);
        return 0;
    }
    if (strcmp(mode, "success") == 0) {
        TEST_CHECK_EQ(0x0411, language);
        TEST_CHECK_EQ(sentinel, error);
    } else {
        TEST_CHECK_EQ(0, language);
        TEST_CHECK_EQ(strcmp(mode, "failed") == 0 ? ERROR_ACCESS_DENIED
                      : strcmp(mode, "unavailable") == 0 ? ERROR_NOT_SUPPORTED : ERROR_INVALID_DATA, error);
    }
    return 0;
}
