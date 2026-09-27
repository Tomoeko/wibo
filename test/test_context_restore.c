#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test_assert.h"

typedef struct {
	uint64_t expectedRsp;
	uint64_t capturedRsp;
	uint64_t capturedRip;
	uint64_t landingAddress;
	uint64_t observedRsp;
	uint64_t observedRax;
	uint64_t observedRbx;
	unsigned char observedXmm6[16];
	DWORD restoreReturned;
	DWORD landed;
	DWORD capturedFlags;
	DWORD observedMxCsr;
	DWORD unsupportedCapturedState;
	DWORD reserved;
} RestoreObservation;

_Static_assert(sizeof(CONTEXT) == 1232, "x64 context size");
_Static_assert(_Alignof(CONTEXT) == 16, "x64 context alignment");
_Static_assert(offsetof(CONTEXT, ContextFlags) == 48, "context flags offset");
_Static_assert(offsetof(CONTEXT, Rax) == 120, "context RAX offset");
_Static_assert(offsetof(CONTEXT, Rbx) == 144, "context RBX offset");
_Static_assert(offsetof(CONTEXT, Rsp) == 152, "context RSP offset");
_Static_assert(offsetof(CONTEXT, Rip) == 248, "context RIP offset");
_Static_assert(offsetof(CONTEXT, FltSave) == 256, "legacy floating-point offset");
_Static_assert(offsetof(CONTEXT, FltSave.XmmRegisters[6]) == 512, "context XMM6 offset");
_Static_assert(offsetof(RestoreObservation, observedXmm6) == 56, "observed XMM6 offset");
_Static_assert(offsetof(RestoreObservation, restoreReturned) == 72, "return marker offset");
_Static_assert(offsetof(RestoreObservation, landed) == 76, "landing marker offset");
_Static_assert(offsetof(RestoreObservation, capturedFlags) == 80, "captured flags offset");
_Static_assert(offsetof(RestoreObservation, observedMxCsr) == 84, "observed MXCSR offset");
_Static_assert(offsetof(RestoreObservation, unsupportedCapturedState) == 88, "precondition marker offset");
_Static_assert(sizeof(RestoreObservation) == 96, "observation size");

void WINAPI wiboContextRestoreProbe(CONTEXT *context, void *capture, void *restore, RestoreObservation *observation);

static CONTEXT context;
static RestoreObservation observation;

static void checkRestore(HMODULE module, void *capture, const char *name) {
	void *restore = (void *)(ULONG_PTR)GetProcAddress(module, "RtlRestoreContext");
	TEST_CHECK(restore != NULL);
	memset(&context, 0, sizeof(context));
	memset(&observation, 0, sizeof(observation));
	wiboContextRestoreProbe(&context, capture, restore, &observation);
	printf("module=%s flags=%lx landed=%lu returned=%lu unsupported=%lu\n", name,
		   (unsigned long)observation.capturedFlags, (unsigned long)observation.landed,
		   (unsigned long)observation.restoreReturned, (unsigned long)observation.unsupportedCapturedState);

	TEST_CHECK_EQ(0, observation.unsupportedCapturedState);
	TEST_CHECK_EQ(0, observation.restoreReturned);
	TEST_CHECK_EQ(1, observation.landed);
	TEST_CHECK_EQ(CONTEXT_FULL | CONTEXT_SEGMENTS, observation.capturedFlags);
	TEST_CHECK_U64_EQ(observation.expectedRsp, observation.capturedRsp);
	TEST_CHECK_U64_EQ(observation.capturedRsp, context.Rsp);
	TEST_CHECK_U64_EQ(observation.capturedRsp, observation.observedRsp);
	TEST_CHECK(observation.capturedRip != 0);
	TEST_CHECK(observation.capturedRip != observation.landingAddress);
	TEST_CHECK_U64_EQ(observation.landingAddress, context.Rip);
	TEST_CHECK_U64_EQ(UINT64_C(0x123456789abcdef0), observation.observedRax);
	TEST_CHECK_U64_EQ(UINT64_C(0x0fedcba987654321), observation.observedRbx);
	const uint64_t expectedXmm6[] = {UINT64_C(0x1020304050607080), UINT64_C(0x90a0b0c0d0e0f000)};
	TEST_CHECK(memcmp(expectedXmm6, observation.observedXmm6, sizeof(expectedXmm6)) == 0);
	TEST_CHECK_EQ(context.FltSave.MxCsr, observation.observedMxCsr);
}

int main(void) {
	HMODULE native = GetModuleHandleA("ntdll.dll");
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(native != NULL && kernel != NULL);
	void *capture = (void *)(ULONG_PTR)GetProcAddress(native, "RtlCaptureContext");
	TEST_CHECK(capture != NULL);
	checkRestore(native, capture, "ntdll");
	checkRestore(kernel, capture, "kernel32");
	return 0;
}
