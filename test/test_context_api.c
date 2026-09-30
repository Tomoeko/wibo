#define _WIN32_WINNT 0x0601
#include <windows.h>

#include "test_assert.h"

#ifndef CONTEXT_XSTATE
#define CONTEXT_XSTATE (CONTEXT_i386 | 0x40)
#endif

int main(void) {
	DWORD length = 0;
	PCONTEXT context = (PCONTEXT)(ULONG_PTR)0x1234;
	SetLastError(77);
	TEST_CHECK(!InitializeContext(NULL, CONTEXT_FULL, &context, &length));
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
	TEST_CHECK(length >= sizeof(CONTEXT));
	TEST_CHECK(context == (PCONTEXT)(ULONG_PTR)0x1234);

	BYTE *allocation = (BYTE *)malloc(length + 32);
	TEST_CHECK(allocation != NULL);
	BYTE *buffer = (BYTE *)(((ULONG_PTR)allocation + 15) & ~(ULONG_PTR)15);
	DWORD shortLength = length - 1;
	SetLastError(79);
	TEST_CHECK(!InitializeContext(buffer, CONTEXT_FULL, &context, &shortLength));
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
	TEST_CHECK_EQ(length, shortLength);
	TEST_CHECK(context == (PCONTEXT)(ULONG_PTR)0x1234);

	DWORD available = length + 16;
	SetLastError(83);
	TEST_CHECK(InitializeContext(buffer, CONTEXT_FULL, &context, &available));
	TEST_CHECK_EQ(83, GetLastError());
	TEST_CHECK_EQ(length, available);
	TEST_CHECK(context != NULL);
	TEST_CHECK(context->ContextFlags == CONTEXT_FULL);

	DWORD64 mask = ~(DWORD64)0;
	TEST_CHECK(GetXStateFeaturesMask(context, &mask));
#ifdef _WIN64
	TEST_CHECK_U64_EQ(XSTATE_MASK_LEGACY, mask);
#else
	TEST_CHECK_U64_EQ(0, mask);
#endif

	DWORD areaLength = 0;
	BYTE *floatingPoint = (BYTE *)LocateXStateFeature(context, 0, &areaLength);
	TEST_CHECK(floatingPoint != NULL);
	TEST_CHECK_EQ(160, areaLength);
#ifdef _WIN64
	TEST_CHECK_EQ(256, floatingPoint - (BYTE *)context);
#else
	TEST_CHECK_EQ(204, floatingPoint - (BYTE *)context);
#endif
	BYTE *sse = (BYTE *)LocateXStateFeature(context, 1, &areaLength);
	TEST_CHECK(sse != NULL);
#ifdef _WIN64
	TEST_CHECK_EQ(256, areaLength);
#else
	TEST_CHECK_EQ(128, areaLength);
#endif
	TEST_CHECK_EQ(160, sse - floatingPoint);
	areaLength = 0xaaaa;
	TEST_CHECK(LocateXStateFeature(context, 2, &areaLength) == NULL);
	TEST_CHECK_EQ(0xaaaa, areaLength);

	SetLastError(89);
	TEST_CHECK(SetXStateFeaturesMask(context, XSTATE_MASK_LEGACY_FLOATING_POINT));
	TEST_CHECK_EQ(89, GetLastError());
	TEST_CHECK(GetXStateFeaturesMask(context, &mask));
	TEST_CHECK_U64_EQ(XSTATE_MASK_LEGACY, mask);
	SetLastError(91);
	TEST_CHECK(!SetXStateFeaturesMask(context, XSTATE_MASK_AVX));
	TEST_CHECK_EQ(91, GetLastError());
	TEST_CHECK(GetXStateFeaturesMask(context, &mask));
	TEST_CHECK_U64_EQ(XSTATE_MASK_LEGACY, mask);

	if (GetEnabledXStateFeatures() == 0) {
		DWORD xstateLength = 0;
		PCONTEXT xstateContext = NULL;
		TEST_CHECK(!InitializeContext(NULL, CONTEXT_FULL | CONTEXT_XSTATE, &xstateContext, &xstateLength));
		TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
		TEST_CHECK_EQ(length, xstateLength);
		xstateLength += 16;
		TEST_CHECK(InitializeContext(buffer, CONTEXT_FULL | CONTEXT_XSTATE, &xstateContext, &xstateLength));
		TEST_CHECK_EQ(length, xstateLength);
		TEST_CHECK_EQ(CONTEXT_FULL, xstateContext->ContextFlags);
	}
	DWORD allLength = length + 16;
	PCONTEXT allContext = NULL;
	TEST_CHECK(InitializeContext(buffer, CONTEXT_ALL, &allContext, &allLength));
	TEST_CHECK_EQ(length, allLength);
	TEST_CHECK(GetXStateFeaturesMask(allContext, &mask));
	TEST_CHECK_U64_EQ(XSTATE_MASK_LEGACY, mask);

	free(allocation);
	return 0;
}
