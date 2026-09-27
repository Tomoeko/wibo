#include "setup.h"
#include "software_exception_decision.h"
#include "test_assert.h"

#include <cstdio>
#include <cstring>

namespace {
using Kind = SoftwareExceptionDecisionKind64;
constexpr DWORD kOuterCode = 0xe0420701;
constexpr DWORD kInnerCode = 0xe0420702;
constexpr DWORD kLegacyThreadNameException = 0x406d1388;
unsigned hostTransitions, guestTransitions, handlerCalls;
bool guestActive;
char order[32];
unsigned orderCount;
PVOID selfToken;
EXCEPTION_RECORD *sharedRecord;
CONTEXT64 *sharedContext;
DWORD sharedMxcsr, sharedSavedMxcsr;

void initializeCapture(SoftwareExceptionCapture64 &capture, DWORD code = kOuterCode) {
	capture = {};
	wiboCaptureContext64(&capture.context);
	TEST_CHECK_EQ(WIBO_CTX64_CAPTURE_FLAGS, capture.context.ContextFlags);
	TEST_CHECK_EQ(0x3f, capture.context.FltSave.ControlWord & 0x3f);
	TEST_CHECK_EQ(0x1f80, capture.context.FltSave.MxCsr & 0x1f80);
	capture.localRecord.ExceptionCode = code;
	capture.localRecord.NumberParameters = 1;
	capture.localRecord.ExceptionInformation[0] = 3;
	capture.record = &capture.localRecord;
}

SoftwareExceptionDecision64 decide(const SoftwareExceptionCapture64 &capture, Kind expected) {
	CONTEXT64 before{};
	std::memcpy(&before, &capture.context, sizeof(before));
	const auto *caller = capture.callerCapture;
	const DWORD originalCode = capture.record->ExceptionCode;
	const DWORD originalFlags = capture.record->ExceptionFlags;
	SoftwareExceptionDecision64 output{};
	TEST_CHECK_EQ(static_cast<DWORD>(expected), wiboPrepareSoftwareExceptionDecision64(&capture, &output));
	TEST_CHECK_EQ(static_cast<DWORD>(expected), static_cast<DWORD>(output.kind));
	TEST_CHECK_EQ(originalCode, output.originalCode);
	TEST_CHECK_EQ(originalFlags, output.originalFlags);
	TEST_CHECK(std::memcmp(&before, &capture.context, sizeof(before)) == 0);
	TEST_CHECK(capture.callerCapture == caller);
#if defined(__APPLE__)
	TEST_CHECK(!guestActive);
	TEST_CHECK_EQ(hostTransitions, guestTransitions);
#endif
	return output;
}

void appendOrder(PEXCEPTION_POINTERS info, char label) {
	TEST_CHECK(orderCount + 1 < sizeof(order));
	const auto *record = fromGuestPtr<const EXCEPTION_RECORD>(info->ExceptionRecord);
	order[orderCount++] = record->ExceptionCode == kInnerCode ? static_cast<char>(label + 'a' - 'A') : label;
	order[orderCount] = 0;
	++handlerCalls;
}

LONG GUEST_STDCALL search(PEXCEPTION_POINTERS info) {
	appendOrder(info, 'A');
	return EXCEPTION_CONTINUE_SEARCH;
}

LONG GUEST_STDCALL accept(PEXCEPTION_POINTERS info) {
	appendOrder(info, 'S');
	return EXCEPTION_CONTINUE_EXECUTION;
}

LONG GUEST_STDCALL changeThenSearch(PEXCEPTION_POINTERS info) {
	auto *record = fromGuestPtr<EXCEPTION_RECORD>(info->ExceptionRecord);
	auto *context = fromGuestPtr<CONTEXT64>(info->ContextRecord);
	TEST_CHECK_EQ(3, record->ExceptionInformation[0]);
	sharedRecord = record;
	sharedContext = context;
	record->ExceptionInformation[0] = 2;
	context->Rax = 0x12345678;
	context->EFlags ^= 1;
	context->MxCsr = (context->MxCsr & ~0x6000) | 0x2000;
	if ((context->FltSave.MxCsrMask & 0x6000) == 0x6000) {
		context->FltSave.MxCsr = (context->FltSave.MxCsr & ~0x6000) | 0x4000;
	}
	sharedMxcsr = context->MxCsr;
	sharedSavedMxcsr = context->FltSave.MxCsr;
	std::memset(&context->FltSave.XmmRegisters[6], 0x3c, sizeof(M128A));
	++handlerCalls;
	return EXCEPTION_CONTINUE_SEARCH;
}

LONG GUEST_STDCALL checkChangesThenAccept(PEXCEPTION_POINTERS info) {
	auto *record = fromGuestPtr<EXCEPTION_RECORD>(info->ExceptionRecord);
	auto *context = fromGuestPtr<CONTEXT64>(info->ContextRecord);
	TEST_CHECK(record == sharedRecord && context == sharedContext);
	TEST_CHECK_EQ(2, record->ExceptionInformation[0]);
	TEST_CHECK_U64_EQ(0x12345678, context->Rax);
	TEST_CHECK_EQ(sharedMxcsr, context->MxCsr);
	TEST_CHECK_EQ(sharedSavedMxcsr, context->FltSave.MxCsr);
	const auto *bytes = reinterpret_cast<const unsigned char *>(&context->FltSave.XmmRegisters[6]);
	for (unsigned i = 0; i < sizeof(M128A); ++i) {
		TEST_CHECK_EQ(0x3c, bytes[i]);
	}
	++handlerCalls;
	return EXCEPTION_CONTINUE_EXECUTION;
}

LONG GUEST_STDCALL removeSelfThenNest(PEXCEPTION_POINTERS info) {
	appendOrder(info, 'C');
	const auto *record = fromGuestPtr<const EXCEPTION_RECORD>(info->ExceptionRecord);
	if (record->ExceptionCode == kInnerCode) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	TEST_CHECK_EQ(1, kernel32::RemoveVectoredExceptionHandler(selfToken));
	TEST_CHECK_EQ(0, kernel32::RemoveVectoredExceptionHandler(selfToken));
	SoftwareExceptionCapture64 nested{};
	initializeCapture(nested, kInnerCode);
#if defined(__APPLE__)
	// Model the separate capture bridge's normal host entry while keeping the
	// real registry and guest callback adapter in this isolated host fixture.
	TEB *teb = enterHostContext();
#endif
	decide(nested, Kind::Resume);
#if defined(__APPLE__)
	enterGuestContext(teb);
#endif
	return EXCEPTION_CONTINUE_SEARCH;
}

enum class Mutation {
	Rip,
	Rsp,
	Cs,
	Ss,
	Ds,
	Es,
	Fs,
	Gs,
	Flags,
	Trap,
	Direction,
	ReservedMxcsr,
	UnknownMxcsrMask,
	UnmaskedX87,
	UnmaskedSse,
	RecordPointer,
	ContextPointer,
	ClearNoncontinuable,
	SetNoncontinuable,
};
Mutation mutation;

LONG GUEST_STDCALL mutateThenAccept(PEXCEPTION_POINTERS info) {
	auto *context = fromGuestPtr<CONTEXT64>(info->ContextRecord);
	auto *record = fromGuestPtr<EXCEPTION_RECORD>(info->ExceptionRecord);
	switch (mutation) {
	case Mutation::Rip:
		++context->Rip;
		break;
	case Mutation::Rsp:
		context->Rsp += 8;
		break;
	case Mutation::Cs:
		context->SegCs ^= 1;
		break;
	case Mutation::Ss:
		context->SegSs ^= 1;
		break;
	case Mutation::Ds:
		context->SegDs ^= 1;
		break;
	case Mutation::Es:
		context->SegEs ^= 1;
		break;
	case Mutation::Fs:
		context->SegFs ^= 1;
		break;
	case Mutation::Gs:
		context->SegGs ^= 1;
		break;
	case Mutation::Flags:
		context->ContextFlags ^= 1;
		break;
	case Mutation::Trap:
		context->EFlags ^= 0x100;
		break;
	case Mutation::Direction:
		context->EFlags ^= 0x400;
		break;
	case Mutation::ReservedMxcsr:
		context->FltSave.MxCsr |= 0x80000000;
		break;
	case Mutation::UnknownMxcsrMask:
		context->FltSave.MxCsr ^= 0x2000;
		break;
	case Mutation::UnmaskedX87:
		context->FltSave.ControlWord &= ~1;
		break;
	case Mutation::UnmaskedSse:
		context->FltSave.MxCsr &= ~0x80;
		break;
	case Mutation::RecordPointer:
		info->ExceptionRecord = 0;
		break;
	case Mutation::ContextPointer:
		info->ContextRecord = 0;
		break;
	case Mutation::ClearNoncontinuable:
		record->ExceptionFlags = 0;
		break;
	case Mutation::SetNoncontinuable:
		record->ExceptionFlags = 1;
		break;
	}
	++handlerCalls;
	return EXCEPTION_CONTINUE_EXECUTION;
}

LONG GUEST_STDCALL mutateThenSearch(PEXCEPTION_POINTERS info) {
	mutateThenAccept(info);
	return EXCEPTION_CONTINUE_SEARCH;
}

LONG GUEST_STDCALL setNotificationCodeThenSearch(PEXCEPTION_POINTERS info) {
	fromGuestPtr<EXCEPTION_RECORD>(info->ExceptionRecord)->ExceptionCode = kLegacyThreadNameException;
	++handlerCalls;
	return EXCEPTION_CONTINUE_SEARCH;
}

void remove(PVOID token) {
	TEST_CHECK(token != nullptr);
	TEST_CHECK_EQ(1, kernel32::RemoveVectoredExceptionHandler(token));
	TEST_CHECK_EQ(0, kernel32::RemoveVectoredExceptionHandler(token));
}

void initializeNotificationCapture(SoftwareExceptionCapture64 &capture, SoftwareExceptionCapture64 &caller,
								   DWORD flags = 0) {
	initializeCapture(capture, kLegacyThreadNameException);
	initializeCapture(caller, kLegacyThreadNameException);
	caller.localRecord.ExceptionFlags = flags;
	capture.record = &caller.localRecord;
	capture.callerCapture = &caller;
}

void checkLegacyNotification() {
	SoftwareExceptionCapture64 capture{}, caller{};
	initializeNotificationCapture(capture, caller);
	auto result = decide(capture, Kind::LegacyNotificationReturn);
	TEST_CHECK_EQ(0, result.failureCode);
	TEST_CHECK_EQ(kLegacyThreadNameException, result.originalCode);
	TEST_CHECK(std::memcmp(&capture.context, &result.resumeContext, sizeof(CONTEXT64)) == 0);

	// Captures without matching Raise ancestry do not inherit the notification policy.
	capture.callerCapture = nullptr;
	decide(capture, Kind::UnsupportedFrameDispatch);
	capture.callerCapture = &caller;
	capture.record = &capture.localRecord;
	decide(capture, Kind::UnsupportedFrameDispatch);
	initializeNotificationCapture(capture, caller, 1);
	decide(capture, Kind::UnsupportedFrameDispatch);

	initializeNotificationCapture(capture, caller);
	CONTEXT64 callerBefore{};
	std::memcpy(&callerBefore, &caller.context, sizeof(callerBefore));
	PVOID token = kernel32::AddVectoredExceptionHandler(0, changeThenSearch);
	TEST_CHECK(token != nullptr);
	result = decide(capture, Kind::LegacyNotificationReturn);
	TEST_CHECK_EQ(0, result.failureCode);
	TEST_CHECK_EQ(2, capture.record->ExceptionInformation[0]);
	TEST_CHECK(std::memcmp(&capture.context, &result.resumeContext, sizeof(CONTEXT64)) == 0);
	TEST_CHECK(std::memcmp(&callerBefore, &caller.context, sizeof(callerBefore)) == 0);
	remove(token);

	token = kernel32::AddVectoredExceptionHandler(0, mutateThenSearch);
	TEST_CHECK(token != nullptr);
	initializeNotificationCapture(capture, caller, 1);
	mutation = Mutation::ClearNoncontinuable;
	result = decide(capture, Kind::UnsupportedFrameDispatch);
	TEST_CHECK_EQ(1, result.originalFlags);
	TEST_CHECK_EQ(0, capture.record->ExceptionFlags);
	initializeNotificationCapture(capture, caller);
	mutation = Mutation::SetNoncontinuable;
	result = decide(capture, Kind::UnsupportedFrameDispatch);
	TEST_CHECK_EQ(0, result.originalFlags);
	TEST_CHECK_EQ(1, capture.record->ExceptionFlags);
	initializeNotificationCapture(capture, caller);
	mutation = Mutation::Rip;
	result = decide(capture, Kind::LegacyNotificationReturn);
	TEST_CHECK(std::memcmp(&capture.context, &result.resumeContext, sizeof(CONTEXT64)) == 0);
	initializeNotificationCapture(capture, caller);
	capture.callerCapture = nullptr;
	decide(capture, Kind::UnsupportedFrameDispatch);
	remove(token);

	// A changed record code cannot convert an ordinary exception into a notification.
	initializeNotificationCapture(capture, caller);
	caller.localRecord.ExceptionCode = kOuterCode;
	token = kernel32::AddVectoredExceptionHandler(0, setNotificationCodeThenSearch);
	TEST_CHECK(token != nullptr);
	result = decide(capture, Kind::UnsupportedFrameDispatch);
	TEST_CHECK_EQ(kOuterCode, result.originalCode);
	TEST_CHECK_EQ(kLegacyThreadNameException, capture.record->ExceptionCode);
	remove(token);

	// Successful handlers retain the normal context-selection and validation path.
	token = kernel32::AddVectoredExceptionHandler(0, mutateThenAccept);
	TEST_CHECK(token != nullptr);
	initializeNotificationCapture(capture, caller);
	mutation = Mutation::Rip;
	decide(capture, Kind::UnsupportedControlTransfer);
	initializeNotificationCapture(capture, caller);
	mutation = Mutation::ClearNoncontinuable;
	decide(capture, Kind::Resume);
	remove(token);
}
} // namespace

namespace wibo {
bool debugEnabled = false;
void debug_log(const char *, ...) {}
} // namespace wibo

// These isolated hooks verify balanced normal-return adapter transitions.
// Operating-system TLS and context restoration are not exercised here.
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
extern "C" void wiboConsumeSoftwareExceptionCapture64(const SoftwareExceptionCapture64 *) {
	TEST_FAIL("Capture entry is outside this decision-only fixture");
}

int main() {
	SoftwareExceptionDecision64 invalid{};
	TEST_CHECK_EQ(static_cast<DWORD>(Kind::InvalidCapture), wiboPrepareSoftwareExceptionDecision64(nullptr, &invalid));
	TEST_CHECK_EQ(0xc000000d, invalid.failureCode);
	SoftwareExceptionCapture64 capture{};
	TEST_CHECK_EQ(static_cast<DWORD>(Kind::InvalidCapture), wiboPrepareSoftwareExceptionDecision64(&capture, &invalid));
	TEST_CHECK_EQ(0, handlerCalls);
	initializeCapture(capture);
	auto result = decide(capture, Kind::UnsupportedFrameDispatch);
	TEST_CHECK_EQ(kOuterCode, result.failureCode);
	TEST_CHECK_EQ(0, handlerCalls);

	PVOID first = kernel32::AddVectoredExceptionHandler(0, changeThenSearch);
	TEST_CHECK(first != nullptr);
	SoftwareExceptionCapture64 ancestry{};
	initializeCapture(ancestry);
	capture.callerCapture = &ancestry;
	CONTEXT64 ancestryBefore{};
	std::memcpy(&ancestryBefore, &ancestry.context, sizeof(ancestryBefore));
	result = decide(capture, Kind::UnsupportedFrameDispatch);
	TEST_CHECK_EQ(2, capture.record->ExceptionInformation[0]);
	initializeCapture(capture);
	capture.callerCapture = &ancestry;
	PVOID last = kernel32::AddVectoredExceptionHandler(0, checkChangesThenAccept);
	TEST_CHECK(last != nullptr);
	const unsigned beforeCalls = handlerCalls;
	result = decide(capture, Kind::Resume);
	TEST_CHECK_EQ(2, handlerCalls - beforeCalls);
	TEST_CHECK_EQ(0, result.failureCode);
	TEST_CHECK_U64_EQ(0x12345678, result.resumeContext.Rax);
	TEST_CHECK_EQ(sharedMxcsr, result.resumeContext.MxCsr);
	TEST_CHECK_EQ(sharedSavedMxcsr, result.resumeContext.FltSave.MxCsr);
	TEST_CHECK_EQ(2, capture.record->ExceptionInformation[0]);
	TEST_CHECK(std::memcmp(&ancestryBefore, &ancestry.context, sizeof(ancestryBefore)) == 0);
	remove(first);
	remove(last);

	initializeCapture(capture);
	first = kernel32::AddVectoredExceptionHandler(0, search);
	last = kernel32::AddVectoredExceptionHandler(0, accept);
	selfToken = kernel32::AddVectoredExceptionHandler(1, removeSelfThenNest);
	TEST_CHECK(first != nullptr && last != nullptr && selfToken != nullptr);
	orderCount = 0;
	decide(capture, Kind::Resume);
	// An active self-removal remains visible to nested dispatch until the
	// outer invocation returns, as measured with software raises on the oracle.
	TEST_CHECK_STR_EQ("CcasAS", order);
	TEST_CHECK_EQ(0, kernel32::RemoveVectoredExceptionHandler(selfToken));
	orderCount = 0;
	decide(capture, Kind::Resume);
	TEST_CHECK_STR_EQ("AS", order);
	remove(first);
	remove(last);
	decide(capture, Kind::UnsupportedFrameDispatch);

	first = kernel32::AddVectoredExceptionHandler(0, mutateThenAccept);
	TEST_CHECK(first != nullptr);
	for (unsigned i = static_cast<unsigned>(Mutation::Rip); i <= static_cast<unsigned>(Mutation::ContextPointer); ++i) {
		initializeCapture(capture);
		mutation = static_cast<Mutation>(i);
		if (mutation == Mutation::UnknownMxcsrMask) {
			capture.context.FltSave.MxCsrMask = 0;
		}
		const Kind expected = mutation == Mutation::Rip || mutation == Mutation::Rsp ? Kind::UnsupportedControlTransfer
																					 : Kind::UnsupportedContextState;
		decide(capture, expected);
	}
	initializeCapture(capture);
	capture.record->ExceptionFlags = 1;
	mutation = Mutation::ClearNoncontinuable;
	result = decide(capture, Kind::NoncontinuableContinuationRequiresDispatch);
	TEST_CHECK_EQ(0xc0000025, result.failureCode);
	TEST_CHECK_EQ(1, result.originalFlags);
	TEST_CHECK_EQ(0, capture.record->ExceptionFlags);
	initializeCapture(capture);
	mutation = Mutation::SetNoncontinuable;
	result = decide(capture, Kind::NoncontinuableContinuationRequiresDispatch);
	TEST_CHECK_EQ(0xc0000025, result.failureCode);
	TEST_CHECK_EQ(0, result.originalFlags);
	TEST_CHECK_EQ(1, capture.record->ExceptionFlags);
	remove(first);
	initializeCapture(capture);
	decide(capture, Kind::UnsupportedFrameDispatch);
	checkLegacyNotification();
	std::puts("decision and actual registry checks passed; no public raise or context restoration exercised");
	return EXIT_SUCCESS;
}
