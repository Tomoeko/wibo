#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef WIBO_SOFTWARE_CAPTURE_HOST_FIXTURE
#include "setup.h"
#include "software_exception_capture.h"
#define PROBE_ABI GUEST_STDCALL
#else
#include <windows.h>
#define PROBE_ABI WINAPI
#endif

#include "test_assert.h"

struct CaptureExpected64 {
	std::uint64_t raiseRip{}, raiseRsp{}, rtlRip{}, rtlRsp{}, rtlRecord{}, rtlRecordAddress{};
};
extern "C" void PROBE_ABI wiboSoftwareCaptureProbe64(void *raise, void *rtlRaise, const ULONG_PTR *arguments,
													 CaptureExpected64 *expected);
#ifdef WIBO_SOFTWARE_CAPTURE_HOST_FIXTURE
extern "C" LONG PROBE_ABI wiboChangingVectoredHandler64(PEXCEPTION_POINTERS info);
extern "C" void PROBE_ABI wiboVectoredAdapterProbe64(void *adapter, void *handler, PEXCEPTION_POINTERS info,
													 CONTEXT *observed);
#endif

static EXCEPTION_RECORD records[2];
static CONTEXT contexts[2];
static unsigned captureCount;
#ifdef WIBO_SOFTWARE_CAPTURE_HOST_FIXTURE
static CONTEXT callerContext;
static unsigned hostTransitions, guestTransitions;

// Isolated hooks exercise capture geometry and record preparation. They do not
// validate operating-system TLS transitions or a runnable exception dispatcher.
extern "C" TEB *enterHostContext() {
	++hostTransitions;
	return nullptr;
}
extern "C" void enterGuestContext(TEB *teb) {
	TEST_CHECK(teb == nullptr);
	++guestTransitions;
}
extern "C" TEB *currentTebForGuestTransition() { return nullptr; }

extern "C" void wiboConsumeSoftwareExceptionCapture64(const SoftwareExceptionCapture64 *capture) {
	TEST_CHECK(captureCount < 2);
	TEST_CHECK(capture->record != nullptr);
	if (captureCount == 0) {
		TEST_CHECK(capture->callerCapture != nullptr);
		TEST_CHECK(capture->record == &capture->callerCapture->localRecord);
		callerContext = capture->callerCapture->context;
	} else {
		TEST_CHECK(capture->callerCapture == nullptr);
	}
	records[captureCount] = *capture->record;
	contexts[captureCount] = capture->context;
	++captureCount;
}
#else
static LONG CALLBACK observeCapture(PEXCEPTION_POINTERS info) {
	if (info->ExceptionRecord->ExceptionCode != 0xe0420601) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	TEST_CHECK(captureCount < 2);
	records[captureCount] = *info->ExceptionRecord;
	contexts[captureCount] = *info->ContextRecord;
	++captureCount;
	return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

int main() {
#ifdef WIBO_SOFTWARE_CAPTURE_HOST_FIXTURE
	void *raise = (void *)wiboCaptureRaiseException64;
	void *rtlRaise = (void *)wiboCaptureRtlRaiseException64;
#else
	HMODULE kernel = GetModuleHandleA("kernel32.dll"), native = GetModuleHandleA("ntdll.dll");
	TEST_CHECK(kernel != nullptr && native != nullptr);
	void *raise = (void *)(ULONG_PTR)GetProcAddress(kernel, "RaiseException");
	void *rtlRaise = (void *)(ULONG_PTR)GetProcAddress(native, "RtlRaiseException");
	PVOID token = AddVectoredExceptionHandler(1, observeCapture);
	TEST_CHECK(token != nullptr);
#endif
	TEST_CHECK(raise != nullptr && rtlRaise != nullptr);
	ULONG_PTR arguments[15];
	for (unsigned i = 0; i < 15; ++i) {
		arguments[i] = 0x9000 + i;
	}
	CaptureExpected64 expected{};
	wiboSoftwareCaptureProbe64(raise, rtlRaise, arguments, &expected);
	TEST_CHECK_EQ(2, captureCount);
	for (unsigned i = 0; i < 2; ++i) {
		TEST_CHECK_EQ(0xe0420601, records[i].ExceptionCode);
		TEST_CHECK_EQ(0, records[i].ExceptionFlags);
		TEST_CHECK_EQ(0, (std::uintptr_t)records[i].ExceptionRecord);
		TEST_CHECK_U64_EQ(contexts[i].Rip, (std::uintptr_t)records[i].ExceptionAddress);
		TEST_CHECK_EQ(0x10000f, contexts[i].ContextFlags);
		TEST_CHECK_U64_EQ(0x2132435465768798, contexts[i].Rbx);
		TEST_CHECK_U64_EQ(0x31425364758697a8, contexts[i].Rbp);
		TEST_CHECK_U64_EQ(0xa1b2c3d4e5f60718, contexts[i].R12);
		TEST_CHECK_U64_EQ(0xb1c2d3e4f5061728, contexts[i].R13);
		TEST_CHECK_U64_EQ(0xc1d2e3f405162738, contexts[i].R14);
		TEST_CHECK_U64_EQ(0xd1e2f30415263748, contexts[i].R15);
		const unsigned char *xmm6 = (const unsigned char *)&contexts[i].FltSave.XmmRegisters[6];
		const unsigned char *xmm15 = (const unsigned char *)&contexts[i].FltSave.XmmRegisters[15];
		for (unsigned byte = 0; byte < 16; ++byte) {
			TEST_CHECK_EQ(0x26, xmm6[byte]);
			TEST_CHECK_EQ(0x3f, xmm15[byte]);
		}
	}
	TEST_CHECK_EQ(15, records[0].NumberParameters);
	TEST_CHECK(std::memcmp(arguments, records[0].ExceptionInformation, sizeof(arguments)) == 0);
	TEST_CHECK(contexts[0].Rip != expected.raiseRip);
	TEST_CHECK(contexts[0].Rsp < expected.raiseRsp);
	TEST_CHECK_EQ(2, records[1].NumberParameters);
	TEST_CHECK_EQ(0x4567, records[1].ExceptionInformation[0]);
	TEST_CHECK_EQ(0x89ab, records[1].ExceptionInformation[1]);
	TEST_CHECK_U64_EQ(expected.rtlRip, contexts[1].Rip);
	TEST_CHECK_U64_EQ(expected.rtlRsp, contexts[1].Rsp);
	TEST_CHECK_U64_EQ(expected.rtlRecord, contexts[1].Rcx);
	TEST_CHECK_U64_EQ(expected.rtlRip, expected.rtlRecordAddress);
#ifdef WIBO_SOFTWARE_CAPTURE_HOST_FIXTURE
	TEST_CHECK_U64_EQ(toGuestPtr(wiboRaiseCaptureContinuation64), contexts[0].Rip);
	TEST_CHECK_U64_EQ(expected.raiseRip, callerContext.Rip);
	TEST_CHECK_U64_EQ(expected.raiseRsp, callerContext.Rsp);
	TEST_CHECK_EQ(0xe0420601, callerContext.Rcx);
	TEST_CHECK_EQ(0x80, callerContext.Rdx);
	TEST_CHECK_EQ(15, callerContext.R8);
	TEST_CHECK_U64_EQ((std::uintptr_t)arguments, callerContext.R9);
#if defined(__APPLE__)
	TEST_CHECK_EQ(3, hostTransitions);
	TEST_CHECK_EQ(3, guestTransitions);
#else
	TEST_CHECK_EQ(0, hostTransitions);
	TEST_CHECK_EQ(0, guestTransitions);
#endif
	EXCEPTION_POINTERS info{};
	CONTEXT observed{};
	wiboVectoredAdapterProbe64((void *)wiboCallVectoredHandler64, (void *)wiboChangingVectoredHandler64, &info,
							   &observed);
	TEST_CHECK_EQ(0xffffffff, observed.Home[0]);
	TEST_CHECK_U64_EQ(0x2132435465768798, observed.Rbx);
	TEST_CHECK_U64_EQ(0x31425364758697a8, observed.Rbp);
	TEST_CHECK_U64_EQ(0xa1b2c3d4e5f60718, observed.R12);
	TEST_CHECK_U64_EQ(0xb1c2d3e4f5061728, observed.R13);
	TEST_CHECK_U64_EQ(0xc1d2e3f405162738, observed.R14);
	TEST_CHECK_U64_EQ(0xd1e2f30415263748, observed.R15);
	TEST_CHECK_U64_EQ((std::uintptr_t)&info, observed.Rsi);
	TEST_CHECK_U64_EQ((std::uintptr_t)wiboChangingVectoredHandler64, observed.Rdi);
	TEST_CHECK_EQ(0x3f80, observed.MxCsr);
	TEST_CHECK_EQ(0x077f, observed.FltSave.ControlWord);
	TEST_CHECK_EQ(0x3000, observed.FltSave.StatusWord & 0x3800);
	for (unsigned i = 0; i < 16; ++i) {
		const unsigned char *xmm = (const unsigned char *)&observed.FltSave.XmmRegisters[i];
		for (unsigned byte = 0; byte < 16; ++byte) {
			TEST_CHECK_EQ(i + 1, xmm[byte]);
		}
	}
	SoftwareExceptionCapture64 prepared{};
	prepared.context.Rcx = 0xe0420601;
	prepared.context.Rdx = 0x80;
	prepared.context.R8 = 15;
	wiboPrepareSoftwareExceptionCapture64(&prepared, TRUE);
	TEST_CHECK_EQ(0, prepared.record->NumberParameters);
	TEST_CHECK_EQ(0, prepared.record->ExceptionFlags);
	std::puts("capture geometry and record preparation passed; TLS hooks isolated, dispatch not exercised");
#else
	TEST_CHECK(RemoveVectoredExceptionHandler(token) != 0);
#endif
	return 0;
}
