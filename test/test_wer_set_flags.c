#include <windows.h>

#include "test_assert.h"

__declspec(dllimport) HRESULT WINAPI WerSetFlags(DWORD dwFlags);

int main(void) {
	const DWORD noHeap = 0x1;
	const DWORD disableThreadSuspension = 0x4;

	TEST_CHECK_EQ(S_OK, WerSetFlags(0));
	TEST_CHECK_EQ(S_OK, WerSetFlags(noHeap | disableThreadSuspension));
	return 0;
}
