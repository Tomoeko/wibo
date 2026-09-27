#include "common.h"
#include "setup.h"
#include "software_exception_dispatch.h"
#include "test_assert.h"

#include <cstring>
#ifdef WIBO_SOFTWARE_DISPATCH_REAL_TEB
#include <ctime>
#include <pthread.h>
#endif

struct DispatchExpected64 {
	std::uint64_t raiseRip, raiseRsp, rtlRip, rtlRsp, rtlRecord;
	std::uint64_t raiseSourceArgument, rtlRecordArgument;
};
static_assert(sizeof(DispatchExpected64) == 56);
extern "C" void GUEST_STDCALL wiboSoftwareDispatchProbe64(void *raise, void *rtlRaise, DispatchExpected64 *expected,
														  CONTEXT64 *observed);

extern "C" {
thread_local TEB *currentThreadTeb = nullptr;
}
namespace wibo {
bool debugEnabled = false;
void debug_log(const char *, ...) {}
} // namespace wibo

namespace {
struct ProbeState {
	DispatchExpected64 expected{};
	CONTEXT64 selected[2]{};
	CONTEXT64 observed[2]{};
	unsigned callbacks = 0;
	unsigned depth = 0;
};
ProbeState *activeState;
unsigned callbackCount;
#ifdef WIBO_SOFTWARE_DISPATCH_REAL_TEB
std::uint64_t hostPebSlot, hostErrorSlot;
std::uint64_t readPebSlot() {
	std::uint64_t result;
	__asm__ volatile("movq %%gs:0x60, %0" : "=r"(result));
	return result;
}
std::uint64_t readErrorSlot() {
	std::uint64_t result;
	__asm__ volatile("movq %%gs:0x68, %0" : "=r"(result));
	return result;
}
void writeErrorSlot(std::uint64_t value) { __asm__ volatile("movq %0, %%gs:0x68" : : "r"(value) : "memory"); }
#else
bool guestActive;
unsigned hostTransitions, guestTransitions;
#endif

void checkObserved(const ProbeState &state) {
	TEST_CHECK_EQ(2, state.callbacks);
	TEST_CHECK_EQ(0x9000, state.expected.raiseSourceArgument);
	TEST_CHECK_EQ(0x4568, state.expected.rtlRecordArgument);
	for (unsigned index = 0; index < 2; ++index) {
		const auto &observed = state.observed[index];
		const auto &selected = state.selected[index];
#define CHECK_SELECTED_GPR(field) TEST_CHECK_U64_EQ(selected.field, observed.field)
		CHECK_SELECTED_GPR(Rax);
		CHECK_SELECTED_GPR(Rcx);
		CHECK_SELECTED_GPR(Rdx);
		CHECK_SELECTED_GPR(Rbx);
		CHECK_SELECTED_GPR(Rbp);
		CHECK_SELECTED_GPR(Rsi);
		CHECK_SELECTED_GPR(Rdi);
		CHECK_SELECTED_GPR(R8);
		CHECK_SELECTED_GPR(R9);
		CHECK_SELECTED_GPR(R10);
		CHECK_SELECTED_GPR(R11);
		CHECK_SELECTED_GPR(R12);
		CHECK_SELECTED_GPR(R13);
		CHECK_SELECTED_GPR(R14);
		CHECK_SELECTED_GPR(R15);
#undef CHECK_SELECTED_GPR
		TEST_CHECK_U64_EQ(index ? state.expected.rtlRsp : state.expected.raiseRsp, observed.Rsp);
		TEST_CHECK_U64_EQ(index ? state.expected.rtlRip : state.expected.raiseRip, observed.Rip);
		TEST_CHECK_EQ(selected.EFlags & 0x8d5, observed.EFlags & 0x8d5);
		TEST_CHECK_EQ(selected.FltSave.MxCsr, observed.MxCsr);
		TEST_CHECK_EQ(selected.FltSave.MxCsr, observed.FltSave.MxCsr);
		TEST_CHECK_EQ(selected.FltSave.ControlWord, observed.FltSave.ControlWord);
		TEST_CHECK_EQ(selected.FltSave.StatusWord & 0x3800, observed.FltSave.StatusWord & 0x3800);
		TEST_CHECK_EQ(selected.FltSave.TagWord, observed.FltSave.TagWord);
		TEST_CHECK(std::memcmp(selected.FltSave.XmmRegisters, observed.FltSave.XmmRegisters,
							   sizeof(selected.FltSave.XmmRegisters)) == 0);
		for (unsigned reg = 0; reg < 2; ++reg) {
			TEST_CHECK(std::memcmp(&selected.FltSave.FloatRegisters[reg], &observed.FltSave.FloatRegisters[reg], 10) ==
					   0);
		}
	}
}

void runProbe(ProbeState &state) {
	activeState = &state;
	wiboSoftwareDispatchProbe64(reinterpret_cast<void *>(&wiboDispatchRaiseException64),
								reinterpret_cast<void *>(&wiboDispatchRtlRaiseException64), &state.expected,
								state.observed);
	checkObserved(state);
}

LONG GUEST_STDCALL handle(PEXCEPTION_POINTERS info) {
#ifdef WIBO_SOFTWARE_DISPATCH_REAL_TEB
	// Read guest mirrors directly before reentering host context. All checks,
	// registry work, nested C++ calls, and libc use follow that transition.
	const auto guestPeb = readPebSlot();
	const auto guestError = readErrorSlot();
	writeErrorSlot(guestError + 1);
#endif
#if defined(__APPLE__)
	TEB *teb = enterHostContext();
#endif
#ifdef WIBO_SOFTWARE_DISPATCH_REAL_TEB
	TEST_CHECK(teb != nullptr && teb == currentThreadTeb && !teb->GuestContextActive);
	TEST_CHECK_U64_EQ(teb->Peb, guestPeb);
	TEST_CHECK_EQ(guestError + 1, teb->LastErrorValue);
	TEST_CHECK_U64_EQ(hostPebSlot, readPebSlot());
	TEST_CHECK_U64_EQ(hostErrorSlot, readErrorSlot());
#endif
	TEST_CHECK(activeState != nullptr && activeState->callbacks < 2);
	ProbeState *state = activeState;
	const unsigned index = state->callbacks++;
	++callbackCount;
	auto *record = fromGuestPtr<EXCEPTION_RECORD>(info->ExceptionRecord);
	auto *selected = fromGuestPtr<CONTEXT64>(info->ContextRecord);
	const auto *capture = reinterpret_cast<const SoftwareExceptionCapture64 *>(
		reinterpret_cast<std::uintptr_t>(selected) - WIBO_SOFTWARE_DISPATCH_FRAME_DECISION +
		WIBO_SOFTWARE_FRAME_CAPTURE);
	TEST_CHECK(record == capture->record);
	TEST_CHECK_EQ(0xe0420801, record->ExceptionCode);
	TEST_CHECK_EQ(0, record->ExceptionFlags);
	TEST_CHECK_U64_EQ(capture->context.Rip, record->ExceptionAddress);
	TEST_CHECK_U64_EQ(0x2132435465768798, capture->context.Rbx);
	TEST_CHECK_U64_EQ(0x31425364758697a8, capture->context.Rbp);
	TEST_CHECK_U64_EQ(0xa1b2c3d4e5f60718, capture->context.R12);
	TEST_CHECK_U64_EQ(0xd1e2f30415263748, capture->context.R15);
	TEST_CHECK_EQ(0x077f, capture->context.FltSave.ControlWord);
	TEST_CHECK_EQ(0x3f80, capture->context.FltSave.MxCsr);
	CONTEXT64 before{};
	std::memcpy(&before, &capture->context, sizeof(before));
	CONTEXT64 callerBefore{};
	if (index == 0) {
		TEST_CHECK(capture->callerCapture != nullptr);
		TEST_CHECK(record == &capture->callerCapture->localRecord);
		TEST_CHECK_U64_EQ(toGuestPtr(wiboRaiseDispatchContinuation64), capture->context.Rip);
		TEST_CHECK_U64_EQ(state->expected.raiseRip, capture->callerCapture->context.Rip);
		TEST_CHECK_U64_EQ(state->expected.raiseRsp, capture->callerCapture->context.Rsp);
		TEST_CHECK_EQ(15, record->NumberParameters);
		for (unsigned i = 0; i < 15; ++i) {
			TEST_CHECK_EQ(0x9000 + i, record->ExceptionInformation[i]);
		}
		std::memcpy(&callerBefore, &capture->callerCapture->context, sizeof(callerBefore));
	} else {
		TEST_CHECK(capture->callerCapture == nullptr);
		TEST_CHECK_U64_EQ(state->expected.rtlRip, capture->context.Rip);
		TEST_CHECK_U64_EQ(state->expected.rtlRsp, capture->context.Rsp);
		TEST_CHECK_U64_EQ(state->expected.rtlRecord, toGuestPtr(record));
		TEST_CHECK_EQ(1, record->NumberParameters);
		TEST_CHECK_EQ(0x4567, record->ExceptionInformation[0]);
	}
	if (state->depth == 0 && index == 0) {
		// The nested probe preserves this callback's native ABI. Its fixed
		// continuation returns normally before the outer traversal continues.
		ProbeState nested{};
		nested.depth = 1;
		runProbe(nested);
		activeState = state;
	}
	record->ExceptionInformation[0] += 1;
	selected->Rax = 0x0102030405060708;
	selected->Rcx = 0x1122334455667788;
	selected->Rdx = 0x2132435465768798;
	selected->Rbx = 0x31425364758697a8;
	selected->Rbp = 0x415263748596a7b8;
	selected->Rsi = 0x5162738495a6b7c8;
	selected->Rdi = 0x61728394a5b6c7d8;
	selected->R8 = 0x718293a4b5c6d7e8;
	selected->R9 = 0x8192a3b4c5d6e7f8;
	selected->R10 = 0x91a2b3c4d5e6f708;
	selected->R11 = 0xa1b2c3d4e5f60718;
	selected->R12 = 0xb1c2d3e4f5061728;
	selected->R13 = 0xc1d2e3f405162738;
	selected->R14 = 0xd1e2f30415263748;
	selected->R15 = 0xe1f2031425364758;
	selected->EFlags = (selected->EFlags & ~0x8d5) | 0x885;
	selected->FltSave.ControlWord = (selected->FltSave.ControlWord & ~0xc00) | 0x800;
	if ((selected->FltSave.MxCsrMask & 0x6000) == 0x6000) {
		selected->FltSave.MxCsr = (selected->FltSave.MxCsr & ~0x6000) | 0x4000;
	}
	selected->MxCsr = selected->FltSave.MxCsr;
	for (unsigned reg = 0; reg < 16; ++reg) {
		std::memset(&selected->FltSave.XmmRegisters[reg], static_cast<int>(0x50 + reg), sizeof(M128A));
	}
	std::memcpy(&state->selected[index], selected, sizeof(*selected));
	TEST_CHECK(std::memcmp(&before, &capture->context, sizeof(before)) == 0);
	if (index == 0) {
		TEST_CHECK(std::memcmp(&callerBefore, &capture->callerCapture->context, sizeof(callerBefore)) == 0);
	}
#if defined(__APPLE__)
	enterGuestContext(teb);
#endif
	return EXCEPTION_CONTINUE_EXECUTION;
}

void checkEntrySetAncestry() {
	SoftwareExceptionCapture64 outer{}, inner{};
	EXCEPTION_RECORD unrelated{};
	inner.context.Rsp = toGuestPtr(&outer) - WIBO_SOFTWARE_FRAME_CAPTURE;
	inner.context.Rcx = toGuestPtr(&outer.localRecord);
	inner.context.Rip = toGuestPtr(wiboRaiseCaptureContinuation64);
	wiboPrepareSoftwareExceptionDispatchCapture64(&inner, FALSE);
	TEST_CHECK(inner.callerCapture == nullptr);
	inner.context.Rip = toGuestPtr(wiboRaiseDispatchContinuation64);
	wiboPrepareSoftwareExceptionCapture64(&inner, FALSE);
	TEST_CHECK(inner.callerCapture == nullptr);
	wiboPrepareSoftwareExceptionDispatchCapture64(&inner, FALSE);
	TEST_CHECK(inner.callerCapture == &outer);
	inner.context.Rcx = toGuestPtr(&unrelated);
	wiboPrepareSoftwareExceptionDispatchCapture64(&inner, FALSE);
	TEST_CHECK(inner.callerCapture == nullptr);
}

void checkDispatch() {
	checkEntrySetAncestry();
	PVOID token = kernel32::AddVectoredExceptionHandler(1, handle);
	TEST_CHECK(token != nullptr);
	ProbeState state{};
	runProbe(state);
	TEST_CHECK_EQ(4, callbackCount);
	TEST_CHECK_EQ(1, kernel32::RemoveVectoredExceptionHandler(token));
	TEST_CHECK_EQ(0, kernel32::RemoveVectoredExceptionHandler(token));
}
#ifdef WIBO_SOFTWARE_DISPATCH_REAL_TEB
void *checkThread(void *) {
	std::time_t now = 1700000000;
	std::tm *cached = std::localtime(&now);
	TEST_CHECK(cached != nullptr);
	const std::tm cachedValue = *cached;
	hostPebSlot = readPebSlot();
	hostErrorSlot = readErrorSlot();
	PEB peb{};
	TEB teb{};
	teb.Peb = toGuestPtr(&peb);
	teb.LastErrorValue = 21;
	TEST_CHECK(tebThreadSetup(&teb));
	currentThreadTeb = &teb;
	checkDispatch();
	TEST_CHECK(!teb.GuestContextActive);
	TEST_CHECK_EQ(21 + callbackCount, teb.LastErrorValue);
	TEST_CHECK_U64_EQ(hostPebSlot, readPebSlot());
	TEST_CHECK_U64_EQ(hostErrorSlot, readErrorSlot());
	std::tm *after = std::localtime(&now);
	TEST_CHECK(after == cached && after->tm_year == cachedValue.tm_year && after->tm_yday == cachedValue.tm_yday);
	TEST_CHECK(tebThreadTeardown(&teb));
	currentThreadTeb = nullptr;
	return nullptr;
}
#endif
} // namespace

#ifndef WIBO_SOFTWARE_DISPATCH_REAL_TEB
// Isolated hooks cover normal-return geometry only, without host TLS claims.
extern "C" TEB *enterHostContext() {
	TEST_CHECK(guestActive);
	guestActive = false;
	++hostTransitions;
	return nullptr;
}
extern "C" void enterGuestContext(TEB *teb) {
	TEST_CHECK(teb == nullptr && !guestActive);
	guestActive = true;
	++guestTransitions;
}
extern "C" TEB *currentTebForGuestTransition() {
	TEST_CHECK(!guestActive);
	return nullptr;
}
#endif

extern "C" void wiboEnterSoftwareDispatchFixtureGuest64() {
#if defined(__APPLE__)
	enterGuestContext(currentThreadTeb);
#endif
}
extern "C" void wiboLeaveSoftwareDispatchFixtureGuest64() {
#if defined(__APPLE__)
#ifdef WIBO_SOFTWARE_DISPATCH_REAL_TEB
	const auto guestPeb = readPebSlot();
	const auto guestError = readErrorSlot();
#endif
	TEB *teb = enterHostContext();
#ifdef WIBO_SOFTWARE_DISPATCH_REAL_TEB
	TEST_CHECK(teb != nullptr && teb == currentThreadTeb && !teb->GuestContextActive);
	TEST_CHECK_U64_EQ(teb->Peb, guestPeb);
	TEST_CHECK_EQ(teb->LastErrorValue, guestError);
	TEST_CHECK_U64_EQ(hostPebSlot, readPebSlot());
	TEST_CHECK_U64_EQ(hostErrorSlot, readErrorSlot());
#else
	TEST_CHECK(teb == nullptr);
#endif
#endif
}
extern "C" void wiboConsumeSoftwareExceptionCapture64(const SoftwareExceptionCapture64 *) {
	TEST_FAIL("Raw capture entry is outside this dispatch fixture");
}
[[noreturn]] void wiboUnsupportedSoftwareExceptionDispatch64(const SoftwareExceptionCapture64 *,
															 const SoftwareExceptionDecision64 *decision) {
	TEST_FAIL("Unexpected unsupported decision %u", static_cast<unsigned>(decision->kind));
}

int main() {
#ifdef WIBO_SOFTWARE_DISPATCH_REAL_TEB
	pthread_t worker;
	TEST_CHECK_EQ(0, pthread_create(&worker, nullptr, checkThread, nullptr));
	void *result = nullptr;
	TEST_CHECK_EQ(0, pthread_join(worker, &result));
	TEST_CHECK(result == nullptr);
	std::puts("internal dispatch and genuine thread storage checks passed");
#else
	checkDispatch();
#if defined(__APPLE__)
	TEST_CHECK(!guestActive);
	TEST_CHECK_EQ(hostTransitions, guestTransitions);
#endif
	std::puts("internal dispatch with isolated transition hooks passed; operating-system TLS unverified");
#endif
	return EXIT_SUCCESS;
}
