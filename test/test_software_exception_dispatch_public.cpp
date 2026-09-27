#include <windows.h>

#include "test_assert.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

struct DispatchExpected64 {
	std::uint64_t raiseRip, raiseRsp, rtlRip, rtlRsp, rtlRecord;
	std::uint64_t raiseSourceArgument, rtlRecordArgument;
};
static_assert(sizeof(DispatchExpected64) == 56);
static_assert(sizeof(CONTEXT) == 1232);
extern "C" void WINAPI wiboSoftwareDispatchProbe64(void *raise, void *rtlRaise, DispatchExpected64 *expected,
												   CONTEXT *observed);

// Public calls use the runtime's existing thread storage. The shared probe
// protects its caller's nonvolatile GPR and legacy FP state before returning.
extern "C" void wiboEnterSoftwareDispatchFixtureGuest64() {}
extern "C" void wiboLeaveSoftwareDispatchFixtureGuest64() {}

namespace {
alignas(16) CONTEXT original[2]{};
alignas(16) CONTEXT selected[2]{};
alignas(16) CONTEXT observed[2]{};
EXCEPTION_RECORD records[2]{};
unsigned callbackCount;
bool extraCallback;

LONG CALLBACK selectState(PEXCEPTION_POINTERS info) {
	if (info->ExceptionRecord->ExceptionCode != 0xe0420801) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	const unsigned index = callbackCount++;
	if (index >= 2) {
		extraCallback = true;
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	// Every callback returns normally. RIP/RSP remain unchanged, and no live
	// callback scope is abandoned when the runtime restores selected state.
	records[index] = *info->ExceptionRecord;
	CONTEXT *context = info->ContextRecord;
	original[index] = *context;
	info->ExceptionRecord->ExceptionInformation[0] += 1;
	context->Rax = 0x0102030405060708;
	context->Rcx = 0x1122334455667788;
	context->Rdx = 0x2132435465768798;
	context->Rbx = 0x31425364758697a8;
	context->Rbp = 0x415263748596a7b8;
	context->Rsi = 0x5162738495a6b7c8;
	context->Rdi = 0x61728394a5b6c7d8;
	context->R8 = 0x718293a4b5c6d7e8;
	context->R9 = 0x8192a3b4c5d6e7f8;
	context->R10 = 0x91a2b3c4d5e6f708;
	context->R11 = 0xa1b2c3d4e5f60718;
	context->R12 = 0xb1c2d3e4f5061728;
	context->R13 = 0xc1d2e3f405162738;
	context->R14 = 0xd1e2f30415263748;
	context->R15 = 0xe1f2031425364758;
	context->EFlags = (context->EFlags & ~0x8d5) | 0x885;
	context->FltSave.ControlWord = (context->FltSave.ControlWord & ~0xc00) | 0x800;
	if ((context->FltSave.MxCsr_Mask & 0x6000) == 0x6000) {
		context->FltSave.MxCsr = (context->FltSave.MxCsr & ~0x6000) | 0x4000;
	}
	context->MxCsr = context->FltSave.MxCsr;
	for (unsigned reg = 0; reg < 16; ++reg) {
		std::memset(&context->FltSave.XmmRegisters[reg], static_cast<int>(0x50 + reg), sizeof(M128A));
	}
	selected[index] = *context;
	return EXCEPTION_CONTINUE_EXECUTION;
}

void checkSelectedState(unsigned index, const DispatchExpected64 &expected) {
	const auto &wanted = selected[index];
	const auto &actual = observed[index];
	// These void APIs do not establish volatile return-register values. Check
	// the nonvolatile registers changed by the selected continuation instead.
#define CHECK_SELECTED_GPR(field) TEST_CHECK_U64_EQ(wanted.field, actual.field)
	CHECK_SELECTED_GPR(Rbx);
	CHECK_SELECTED_GPR(Rbp);
	CHECK_SELECTED_GPR(Rsi);
	CHECK_SELECTED_GPR(Rdi);
	CHECK_SELECTED_GPR(R12);
	CHECK_SELECTED_GPR(R13);
	CHECK_SELECTED_GPR(R14);
	CHECK_SELECTED_GPR(R15);
#undef CHECK_SELECTED_GPR
	TEST_CHECK_U64_EQ(index ? expected.rtlRip : expected.raiseRip, actual.Rip);
	TEST_CHECK_U64_EQ(index ? expected.rtlRsp : expected.raiseRsp, actual.Rsp);
	TEST_CHECK(wanted.Rip == original[index].Rip && wanted.Rsp == original[index].Rsp);
	TEST_CHECK_U64_EQ(original[index].Rip, reinterpret_cast<std::uintptr_t>(records[index].ExceptionAddress));
	TEST_CHECK_EQ(wanted.FltSave.ControlWord, actual.FltSave.ControlWord);
	TEST_CHECK_EQ(wanted.FltSave.MxCsr, actual.MxCsr);
	TEST_CHECK_EQ(wanted.FltSave.MxCsr, actual.FltSave.MxCsr);
	for (unsigned reg = 6; reg < 16; ++reg) {
		TEST_CHECK(std::memcmp(&wanted.FltSave.XmmRegisters[reg], &actual.FltSave.XmmRegisters[reg], sizeof(M128A)) ==
				   0);
	}
	if (index == 1) {
		TEST_CHECK_EQ(wanted.EFlags & 0x8d5, actual.EFlags & 0x8d5);
	}
	// RaiseException may modify arithmetic flags in its ordinary epilogue.
	std::printf("%s selected continuation checks passed\n", index ? "RtlRaiseException" : "RaiseException");
}
} // namespace

int main() {
	HMODULE kernel = GetModuleHandleA("kernel32.dll"), native = GetModuleHandleA("ntdll.dll");
	TEST_CHECK(kernel != nullptr && native != nullptr);
	void *raise = reinterpret_cast<void *>(GetProcAddress(kernel, "RaiseException"));
	void *rtlRaise = reinterpret_cast<void *>(GetProcAddress(native, "RtlRaiseException"));
	TEST_CHECK(raise != nullptr && rtlRaise != nullptr);
	PVOID token = AddVectoredExceptionHandler(1, selectState);
	TEST_CHECK(token != nullptr);
	DispatchExpected64 expected{};
	wiboSoftwareDispatchProbe64(raise, rtlRaise, &expected, observed);
	TEST_CHECK(RemoveVectoredExceptionHandler(token) != 0);
	TEST_CHECK_EQ(2, callbackCount);
	TEST_CHECK(!extraCallback);
	for (unsigned index = 0; index < 2; ++index) {
		checkSelectedState(index, expected);
	}
	TEST_CHECK_EQ(0x9000, expected.raiseSourceArgument);
	TEST_CHECK_EQ(0x4568, expected.rtlRecordArgument);
	return EXIT_SUCCESS;
}
