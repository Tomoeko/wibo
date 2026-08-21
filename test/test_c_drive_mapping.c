#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

int main(void) {
	char current[MAX_PATH];
	DWORD currentLength = GetCurrentDirectoryA(sizeof(current), current);
	TEST_CHECK(currentLength > 0 && currentLength < sizeof(current));
	TEST_CHECK(_strnicmp(current, "C:\\", 3) == 0);

	char full[MAX_PATH];
	DWORD fullLength = GetFullPathNameA("test_assert.h", sizeof(full), full, NULL);
	TEST_CHECK(fullLength > 0 && fullLength < sizeof(full));
	TEST_CHECK(_strnicmp(full, "C:\\", 3) == 0);
	TEST_CHECK(strstr(full, "test_assert.h") != NULL);

	return EXIT_SUCCESS;
}
