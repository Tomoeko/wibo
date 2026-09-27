#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef WIBO_CONTEXT_HOST_FIXTURE
#include "context_x64.h"
#define PROBE_ABI GUEST_STDCALL
#else
#include <windows.h>
#define PROBE_ABI WINAPI
#endif

#include "test_assert.h"

#ifdef __cplusplus
extern "C"
#endif
	void PROBE_ABI wiboContextProbe(void *capture, void *restore, CONTEXT *captured, CONTEXT *restored,
									unsigned options);

static CONTEXT captured, restored;

static uint64_t readInteger(const CONTEXT *context, size_t offset) {
	uint64_t value;
	memcpy(&value, (const unsigned char *)context + offset, sizeof(value));
	return value;
}

static void checkContext(unsigned options) {
#ifdef WIBO_CONTEXT_HOST_FIXTURE
	void *capture = (void *)wiboCaptureContext64;
#ifdef WIBO_CONTEXT_CAPTURE_ONLY
	void *restore = NULL;
#else
	void *restore = (void *)wiboRestoreContext64;
#endif
#else
	HMODULE module = GetModuleHandleA("ntdll.dll");
	TEST_CHECK(module != NULL);
	void *capture = (void *)(ULONG_PTR)GetProcAddress(module, "RtlCaptureContext");
#ifdef WIBO_CONTEXT_CAPTURE_ONLY
	void *restore = NULL;
#else
	void *restore = (void *)(ULONG_PTR)GetProcAddress(module, "RtlRestoreContext");
#endif
#endif
	TEST_CHECK(capture != NULL);
#ifndef WIBO_CONTEXT_CAPTURE_ONLY
	TEST_CHECK(restore != NULL);
#endif
	wiboContextProbe(capture, restore, &captured, &restored, options);
	TEST_CHECK_EQ(0x10000f, captured.ContextFlags);
	TEST_CHECK_U64_EQ(readInteger(&captured, 0), captured.Rsp);
	TEST_CHECK_U64_EQ(readInteger(&captured, 8), captured.Rip);
	TEST_CHECK_U64_EQ((uintptr_t)&captured, captured.Rcx);
	TEST_CHECK_EQ(0x41, captured.EFlags & 0x41);
	TEST_CHECK_EQ(options ? 0x5f80 : 0x3f80, captured.MxCsr);
	TEST_CHECK_EQ(0x3f80, captured.FltSave.MxCsr);
	TEST_CHECK_EQ(0x077f, captured.FltSave.ControlWord);
	TEST_CHECK_EQ(0x3000, captured.FltSave.StatusWord & 0x3800);
	const size_t offsets[] = {offsetof(CONTEXT, Rax), offsetof(CONTEXT, Rdx), offsetof(CONTEXT, Rbx),
							  offsetof(CONTEXT, Rbp), offsetof(CONTEXT, Rsi), offsetof(CONTEXT, Rdi),
							  offsetof(CONTEXT, R8),  offsetof(CONTEXT, R9),  offsetof(CONTEXT, R10),
							  offsetof(CONTEXT, R11), offsetof(CONTEXT, R12), offsetof(CONTEXT, R13),
							  offsetof(CONTEXT, R14), offsetof(CONTEXT, R15)};
	const uint64_t values[] = {0x0102030405060708, 0x1122334455667788, 0x2132435465768798, 0x31425364758697a8,
							   0x415263748596a7b8, 0x5162738495a6b7c8, 0x61728394a5b6c7d8, 0x718293a4b5c6d7e8,
							   0x8192a3b4c5d6e7f8, 0x91a2b3c4d5e6f708, 0xa1b2c3d4e5f60718, 0xb1c2d3e4f5061728,
							   0xc1d2e3f405162738, 0xd1e2f30415263748};
	for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
		TEST_CHECK_U64_EQ(values[i], readInteger(&captured, offsets[i]));
	}
	TEST_CHECK_U64_EQ(0, captured.FltSave.FloatRegisters[0].Low);
	TEST_CHECK_EQ(0, captured.FltSave.FloatRegisters[0].High & 0xffff);
	TEST_CHECK_U64_EQ(0x8000000000000000, captured.FltSave.FloatRegisters[1].Low);
	TEST_CHECK_EQ(0x3fff, captured.FltSave.FloatRegisters[1].High & 0xffff);
	for (unsigned i = 0; i < 16; ++i) {
		const unsigned char *xmm = (const unsigned char *)&captured.FltSave.XmmRegisters[i];
		for (unsigned byte = 0; byte < 16; ++byte) {
			TEST_CHECK_EQ(i + 1, xmm[byte]);
		}
	}
#ifndef WIBO_CONTEXT_CAPTURE_ONLY
	TEST_CHECK_U64_EQ(captured.Rsp, restored.Rsp);
	TEST_CHECK_U64_EQ(captured.Rip, restored.Rip);
	TEST_CHECK_EQ(captured.EFlags & 0x8d5, restored.EFlags & 0x8d5);
	TEST_CHECK_EQ(captured.FltSave.MxCsr, restored.MxCsr);
	TEST_CHECK_EQ(captured.FltSave.ControlWord, restored.FltSave.ControlWord);
	TEST_CHECK_EQ(captured.FltSave.StatusWord, restored.FltSave.StatusWord);
	TEST_CHECK_EQ(captured.FltSave.TagWord, restored.FltSave.TagWord);
	const size_t first = offsetof(CONTEXT, Rax), last = offsetof(CONTEXT, Rip);
	TEST_CHECK(
		memcmp((const unsigned char *)&captured + first, (const unsigned char *)&restored + first, last - first) == 0);
	TEST_CHECK(memcmp(captured.FltSave.XmmRegisters, restored.FltSave.XmmRegisters,
					  sizeof(captured.FltSave.XmmRegisters)) == 0);
	for (unsigned i = 0; i < 2; ++i) {
		TEST_CHECK(memcmp(&captured.FltSave.FloatRegisters[i], &restored.FltSave.FloatRegisters[i], 10) == 0);
	}
	TEST_CHECK_EQ(captured.SegCs, restored.SegCs);
	TEST_CHECK_EQ(captured.SegSs, restored.SegSs);
	TEST_CHECK_EQ(restored.MxCsr, restored.FltSave.MxCsr);
#endif
}

int main(void) {
	checkContext(0);
#ifndef WIBO_CONTEXT_CAPTURE_ONLY
	checkContext(1);
#endif
	return 0;
}
