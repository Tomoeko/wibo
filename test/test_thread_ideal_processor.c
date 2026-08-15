#include <windows.h>

#include "test_assert.h"

int main(void) {
	SetThreadIdealProcessor(GetCurrentThread(), 0);
	TEST_CHECK(SetThreadIdealProcessor(GetCurrentThread(), 0) == 0);
	return 0;
}
