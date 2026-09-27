#include "test_assert.h"
#include <windows.h>

static const DWORD errorSeed = 0x4321;

static void checkAllocation(BOOL global, SIZE_T requested, BOOL zero) {
	const UINT flags = zero ? LMEM_ZEROINIT : LMEM_FIXED;
	SetLastError(errorSeed);
	HANDLE memory = global ? GlobalAlloc(flags, requested) : LocalAlloc(flags, requested);
	TEST_CHECK(memory != NULL);
	TEST_CHECK_EQ(errorSeed, GetLastError());

	SetLastError(errorSeed);
	const SIZE_T originalSize = global ? GlobalSize(memory) : LocalSize(memory);
	TEST_CHECK_EQ(errorSeed, GetLastError());
	TEST_CHECK(originalSize >= requested);
	if (zero) {
		for (SIZE_T index = 0; index < requested; ++index)
			TEST_CHECK_EQ(0, ((const BYTE *)memory)[index]);
	}
	memset(memory, 0x5a, requested);

	const SIZE_T nextRequested = requested + 73;
	SetLastError(errorSeed);
	HANDLE next = global ? GlobalReAlloc(memory, nextRequested, GMEM_MOVEABLE | GMEM_ZEROINIT)
						 : LocalReAlloc(memory, nextRequested, LMEM_MOVEABLE | LMEM_ZEROINIT);
	const DWORD reallocError = GetLastError();
	if (!next) {
		if (global)
			GlobalFree(memory);
		else
			LocalFree(memory);
		TEST_FAIL("Fixed reallocation failed with error %lu", (unsigned long)reallocError);
	}
	memory = next;
	TEST_CHECK_EQ(errorSeed, reallocError);

	SetLastError(errorSeed);
	const SIZE_T nextSize = global ? GlobalSize(memory) : LocalSize(memory);
	TEST_CHECK_EQ(errorSeed, GetLastError());
	TEST_CHECK(nextSize >= nextRequested);
	for (SIZE_T index = 0; index < requested; ++index)
		TEST_CHECK_EQ(0x5a, ((const BYTE *)memory)[index]);
	for (SIZE_T index = originalSize; index < nextRequested; ++index)
		TEST_CHECK_EQ(0, ((const BYTE *)memory)[index]);

	SetLastError(errorSeed);
	const HANDLE released = global ? GlobalFree(memory) : LocalFree(memory);
	TEST_CHECK(released == NULL);
	TEST_CHECK_EQ(errorSeed, GetLastError());
}

int main(void) {
	const SIZE_T sizes[] = {0, 1, 17, 257, 4097};
	for (BOOL global = FALSE; global <= TRUE; ++global) {
		SetLastError(errorSeed);
		const SIZE_T size = global ? GlobalSize(NULL) : LocalSize(NULL);
		TEST_CHECK_EQ(0, size);
		TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());

		SetLastError(errorSeed);
		const HANDLE released = global ? GlobalFree(NULL) : LocalFree(NULL);
		TEST_CHECK(released == NULL);
		TEST_CHECK_EQ(errorSeed, GetLastError());

		for (unsigned index = 0; index < sizeof(sizes) / sizeof(sizes[0]); ++index)
			for (BOOL zero = FALSE; zero <= TRUE; ++zero)
				checkAllocation(global, sizes[index], zero);
	}
	puts("Local and global allocation size checks passed");
	return 0;
}
